package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/webrtc/v4"
)

var version = "unreleased"
var sourceDigest = "unreleased"

const pionVersion = "v4.2.24"

type config struct {
	SchemaVersion int    `json:"schema_version"`
	Generation    uint64 `json:"generation,string"`
	CameraMask    uint32 `json:"camera_mask"`
	Width         int    `json:"width"`
	Height        int    `json:"height"`
	FPS           int    `json:"fps"`
	Bitrate       uint64 `json:"bitrate_bps"`
	Codec         string `json:"codec"`
	RtspEnabled   bool   `json:"rtsp_enabled"`
	WebEnabled    bool   `json:"web_enabled"`
	ListenAddress string `json:"listen_address"`
	Port          int    `json:"port"`
	ICEPort       int    `json:"ice_port"`
	SourceRate    int    `json:"source_rate_hz"`
	TelemetryHz   int    `json:"telemetry_limit_hz"`
	MaxVideo      int    `json:"max_video_sessions"`
	MaxIMU        int    `json:"max_imu_clients"`
}
type service struct {
	config             config
	hub                *mediaHub
	telemetry          *telemetryHub
	control            *net.UnixConn
	controlWriteMu     sync.Mutex
	recording          recordingCache
	sourceBits         atomic.Uint32
	sourceAt           atomic.Int64
	producerIMUDrops   atomic.Uint64
	producerVideoDrops [4]atomic.Uint64
}

func randomID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}
func unixFD(fd uintptr) (*net.UnixConn, error) {
	f := os.NewFile(fd, "ipc")
	c, e := net.FileConn(f)
	f.Close()
	if e != nil {
		return nil, e
	}
	u, ok := c.(*net.UnixConn)
	if !ok {
		c.Close()
		return nil, errors.New("not Unix datagram")
	}
	return u, nil
}
func main() {
	if len(os.Args) == 2 && os.Args[1] == "--version" {
		fmt.Printf("sensor_web %s Pion %s source-sha256=%s\n", version, pionVersion, sourceDigest)
		return
	}
	if len(os.Args) != 2 || os.Args[1] != "--inherited-fds" {
		log.Fatal("sensor_web is owned by sensor_demo; use sensor_demo --web")
	}
	if err := run(); err != nil {
		log.Printf("web=degraded: %v", err)
		os.Exit(1)
	}
}
func run() error {
	var c config
	f := os.NewFile(3, "config")
	err := json.NewDecoder(io.LimitReader(f, 4096)).Decode(&c)
	f.Close()
	if err != nil {
		return err
	}
	if c.SchemaVersion != 1 || c.Generation == 0 || c.CameraMask == 0 || c.CameraMask > 15 || net.ParseIP(c.ListenAddress) == nil || c.Port < 1 || c.Port > 65535 || c.ICEPort != 8189 || c.MaxVideo < 1 || c.MaxVideo > 8 || c.MaxIMU < 1 || c.MaxIMU > 2 || c.TelemetryHz < 1 || c.TelemetryHz > 50 {
		return errors.New("invalid inherited configuration")
	}
	video, err := unixFD(4)
	if err != nil {
		return err
	}
	defer video.Close()
	imu, err := unixFD(5)
	if err != nil {
		return err
	}
	defer imu.Close()
	control, err := unixFD(6)
	if err != nil {
		return err
	}
	defer control.Close()
	life := os.NewFile(7, "lifetime")
	defer life.Close()
	httpListener, err := net.Listen("tcp4", net.JoinHostPort(c.ListenAddress, strconv.Itoa(c.Port)))
	if err != nil {
		return err
	}
	defer httpListener.Close()
	udp, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP(c.ListenAddress), Port: c.ICEPort})
	if err != nil {
		return err
	}
	defer udp.Close()
	tcp, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.ParseIP(c.ListenAddress), Port: c.ICEPort})
	if err != nil {
		return err
	}
	defer tcp.Close()
	h := newHub(c)
	packetConn, err := newUDPPacketConn(udp, h)
	if err != nil { return err }
	udpMux := webrtc.NewICEUDPMux(nil, packetConn)
	defer udpMux.Close()
	tcpMux := ice.NewTCPMuxDefault(ice.TCPMuxParams{Listener: tcp, ReadBufferSize: 8, WriteBufferSize: 256 * 1024, FirstStunBindTimeout: 2 * time.Second, AliveDurationForConnFromStun: 5 * time.Second})
	defer tcpMux.Close()
	// All privileged binds are complete; pre-opened IPC needs no directory permission.
	if os.Geteuid() == 0 {
		if err := syscall.Setgroups([]int{}); err != nil {
			return err
		}
		if err := syscall.Setgid(65534); err != nil {
			return err
		}
		if err := syscall.Setuid(65534); err != nil {
			return err
		}
	}
	s := &service{config: c, hub: h, control: control}
	s.telemetry = newTelemetry(c)
	h.udpMux = udpMux
	h.tcpMux = tcpMux
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer cancel()
	go func() { var b [1]byte; _, _ = life.Read(b[:]); cancel() }()
	go h.videoLoop(video)
	go s.telemetry.readLoop(imu)
	go s.controlReadLoop()
	go s.controlLoop(ctx)
	server := &http.Server{Handler: s, ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 15 * time.Second, IdleTimeout: 10 * time.Second, MaxHeaderBytes: 16 * 1024}
	done := make(chan error, 1)
	go func() { done <- server.Serve(httpListener) }()
	select {
	case <-ctx.Done():
	case e := <-done:
		if !errors.Is(e, http.ErrServerClosed) {
			cancel()
		}
	}
	_ = server.Close()
	h.closeAll()
	return nil
}
func (s *service) controlReadLoop() {
	if s.control == nil { return }
	defer func() {
		s.recording.mu.Lock()
		s.recording.at = time.Time{}
		s.recording.mu.Unlock()
	}()
	var buffer [recordingMaxDatagram]byte
	for {
		n, _, flags, _, err := s.control.ReadMsgUnix(buffer[:], nil)
		if err != nil { return }
		if flags&syscall.MSG_TRUNC != 0 { continue }
		b := buffer[:n]
		if n >= 16 && be.Uint32(b) == recordingStatusMagic {
			s.receiveRecordingStatus(b)
			continue
		}
		if n != 64 || !validHeader(b, statusMagic, 64, s.config.Generation) { continue }
		s.sourceBits.Store(be.Uint32(b[16:]))
		s.sourceAt.Store(monotonicNS())
		s.producerIMUDrops.Store(be.Uint64(b[24:]))
		for i := range 4 {
			s.producerVideoDrops[i].Store(be.Uint64(b[32+8*i:]))
		}
	}
}
func (s *service) controlLoop(ctx context.Context) {
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	send := func() {
		var mask uint32
		s.hub.mu.Lock()
		for _, v := range s.hub.sessions {
			mask |= 1 << v.camera
		}
		s.hub.mu.Unlock()
		if s.telemetry.clients.Load() > 0 {
			mask |= 16
		}
		s.hub.demand.Store(mask)
		b := make([]byte, 32)
		be.PutUint32(b, controlMagic)
		be.PutUint16(b[4:], 1)
		be.PutUint16(b[6:], 32)
		be.PutUint64(b[8:], s.config.Generation)
		be.PutUint32(b[24:], mask)
		_ = s.writeControl(b)
	}
	send()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			send()
			s.hub.expireAssemblies()
			s.hub.expireSessions()
		}
	}
}
func jsonError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": code, "message": message}})
}
func (s *service) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; media-src 'self' blob:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
	w.Header().Set("X-Frame-Options", "DENY")
	w.Header().Set("Cache-Control", "no-store")
	host := r.Host
	if h, p, e := net.SplitHostPort(host); e == nil {
		if p != strconv.Itoa(s.config.Port) {
			jsonError(w, 400, "host", "invalid host")
			return
		}
		host = h
	}
	// The accepted socket is authoritative even when an interface appears after startup.
	local, ok := r.Context().Value(http.LocalAddrContextKey).(*net.TCPAddr)
	if !ok || local.IP.To4() == nil || local.IP.IsUnspecified() || host != local.IP.String() || local.Port != s.config.Port || r.URL.RawQuery != "" {
		jsonError(w, 400, "host", "invalid host or query")
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		u, e := url.Parse(origin)
		if e != nil || u.Scheme != "http" || u.Host != r.Host || u.Path != "" {
			jsonError(w, 400, "origin", "same origin required")
			return
		}
	}
	if r.Header.Get("Sec-Fetch-Site") == "cross-site" {
		jsonError(w, 400, "origin", "same origin required")
		return
	}
	switch {
	case r.URL.Path == "/api/v1/info":
		if r.Method != "GET" {
			jsonError(w, 405, "method", "GET required")
			return
		}
		cameras := make([]map[string]any, 0, 4)
		for i := range 4 {
			cameras = append(cameras, map[string]any{"camera_id": i, "enabled": s.config.CameraMask&(1<<i) != 0, "sessions": fmt.Sprintf("/api/v1/cameras/%d/sessions", i)})
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"application_session_id": strconv.FormatUint(s.config.Generation, 10), "version": version, "pion_version": pionVersion, "camera_mask": s.config.CameraMask, "width": s.config.Width, "height": s.config.Height, "fps": s.config.FPS, "bitrate_bps": s.config.Bitrate, "codec": s.config.Codec, "web_enabled": true, "rtsp_enabled": s.config.RtspEnabled, "cameras": cameras, "source_rate_hz": s.config.SourceRate, "telemetry_limit_hz": s.config.TelemetryHz, "accel_unit": "m/s²", "gyro_unit": "rad/s"})
	case r.URL.Path == "/api/v1/status":
		s.status(w, r)
	case r.URL.Path == "/api/v1/recording":
		s.serveRecording(w, r)
	case r.URL.Path == "/api/v1/imu/events":
		s.telemetry.serve(w, r)
	case strings.HasPrefix(r.URL.Path, "/api/v1/cameras/"):
		s.createSession(w, r)
	case strings.HasPrefix(r.URL.Path, "/api/v1/video-sessions/"):
		s.deleteSession(w, r)
	default:
		serveAsset(w, r)
	}
}
func (s *service) status(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		jsonError(w, 405, "method", "GET required")
		return
	}
	bits := s.sourceBits.Load()
	last := s.sourceAt.Load()
	stale := last == 0 || monotonicNS()-last > int64(500*time.Millisecond)
	names := []string{"capture", "encoding", "saving"}
	result := map[string]any{}
	for i, name := range names {
		state := "idle"
		if bits&(1<<i) != 0 {
			state = "flowing"
		}
		if bits&8 != 0 {
			state = "degraded"
		}
		if stale { state = "stale" }
		result[name] = state
	}
	rtsp := "disabled"
	if s.config.RtspEnabled {
		rtsp = "enabled"
		if bits&16 != 0 {
			rtsp = "degraded"
		}
		if stale { rtsp = "stale" }
	}
	result["rtsp"] = rtsp
	result["telemetry"] = s.telemetry.state()
	result["application_session_id"] = strconv.FormatUint(s.config.Generation, 10)
	cameras := make([]map[string]any, 4)
	for i := range 4 {
		c := &s.hub.cameras[i]
		c.mu.Lock()
		state := "idle"
		if s.config.CameraMask&(1<<i) == 0 {
			state = "disabled"
		} else if s.config.Codec != "h264" {
			state = "unsupported"
		} else if s.hub.demand.Load()&(1<<i) != 0 {
			state = "waiting-keyframe"
			if !c.needIDR && time.Since(c.last) < 500*time.Millisecond {
				state = "flowing"
			}
		}
		cameras[i] = map[string]any{"camera_id": i, "web": state, "ipc_drops": strconv.FormatUint(c.drops+s.producerVideoDrops[i].Load(), 10)}
		c.mu.Unlock()
	}
	result["cameras"] = cameras
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(result)
}
