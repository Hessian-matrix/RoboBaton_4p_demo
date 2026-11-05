package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/nack"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/rtp/codecs"
	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

type videoSession struct {
	id         string
	camera     int
	hub        *mediaHub
	pc         *webrtc.PeerConnection
	track      *webrtc.TrackLocalStaticRTP
	mu         sync.Mutex
	queue      []*accessUnit
	bytes      int
	needIDR    bool
	closing    bool
	profile    string
	notify     chan struct{}
	done       chan struct{}
	finished   chan struct{}
	created    time.Time
	connected  atomic.Bool
	writing    atomic.Int64
	lastMedia  atomic.Int64
	ssrc       uint32
	udpLoss    atomic.Uint64
	congestion int
}

// Each primary media worker may send at twice the configured average bitrate,
// with four packets of burst credit. The separate NACK cache holds 512 packets.
// Waiting is private to this worker, never the shared UDP mux.
type mediaPacer struct {
	next time.Time
	rate uint64
	timer *time.Timer
}
func (p *mediaPacer) wait(done <-chan struct{}, bytes int, deadline time.Time) bool {
	now := time.Now()
	if !now.Before(deadline) { return false }
	if p.next.Before(now) { p.next = now }
	burst := time.Duration(uint64(4*1222)*uint64(time.Second)/p.rate)
	ready := p.next.Add(time.Duration(uint64(bytes)*uint64(time.Second)/p.rate))
	if delay := ready.Sub(now)-burst; delay > 0 {
		if !ready.Add(-burst).Before(deadline) { return false }
		p.timer.Reset(delay)
		select {
		case <-done: p.timer.Stop(); return false
		case <-p.timer.C:
		}
	}
	select { case <-done: return false; default: }
	if !time.Now().Before(deadline) { return false }
	p.next = ready
	return true
}

func (v *videoSession) enqueue(a *accessUnit) {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.closing || !v.connected.Load() {
		return
	}
	if a.profile != v.profile {
		go v.close()
		return
	}
	// A paced IDR can occupy the sender for almost maxAge. Retain the
	// dependent frames arriving in that window, still bounded by bytes/age.
	queueLimit := max(4, int((maxAge*time.Duration(v.hub.config.FPS)+time.Second-1)/time.Second))
	if len(v.queue) >= queueLimit || v.bytes+len(a.data) > 4*1024*1024 || time.Since(a.arrived) > maxAge {
		for _, old := range v.queue {
			old.release()
		}
		v.queue = nil
		v.bytes = 0
		v.needIDR = true
		v.congestion++
		if v.congestion >= 20 {
			go v.close()
			return
		}
	}
	if v.needIDR && !a.key {
		return
	}
	a.refs.Add(1)
	v.queue = append(v.queue, a)
	v.bytes += len(a.data)
	select {
	case v.notify <- struct{}{}:
	default:
	}
}
// Revoke delivery synchronously; transport shutdown must never run under hub/queue locks.
func (v *videoSession) revoke() bool {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.closing { return false }
	v.closing = true
	close(v.done)
	for _, a := range v.queue { a.release() }
	v.queue = nil
	v.bytes = 0
	return true
}
func (v *videoSession) close() {
	if v.revoke() { v.finishClose() }
}
func (v *videoSession) finishClose() {
	v.mu.Lock()
	pc := v.pc
	v.mu.Unlock()
	if pc != nil {
		// UDP writes are nonblocking; abort per-peer I/O before Unbind takes
		// the track's exclusive lock. No shared UDP deadline is used.
		_ = pc.SCTP().Transport().ICETransport().Stop()
		_ = pc.Close()
	}
	// Closing sessions continue to consume quota until their transport is released.
	v.hub.mu.Lock()
	if v.hub.sessions[v.id] == v { delete(v.hub.sessions, v.id) }
	v.hub.mu.Unlock()
}
func (v *videoSession) worker() {
	defer close(v.finished)
	payloader := &codecs.H264Payloader{}
	timer := time.NewTimer(time.Hour)
	timer.Stop()
	defer timer.Stop()
	pacer := mediaPacer{rate: uint64(max(v.hub.config.Bitrate/4, 125000)), timer: timer}
	sequence := uint16(time.Now().UnixNano())
	var previous uint64
	var base uint64
	var lastUDPLoss uint64
	for {
		select {
		case <-v.done:
			return
		case <-v.notify:
		}
		for {
			v.mu.Lock()
			if v.closing || len(v.queue) == 0 {
				v.mu.Unlock()
				break
			}
			if loss := v.udpLoss.Load(); loss != lastUDPLoss {
				v.needIDR = true
				lastUDPLoss = loss
			}
			a := v.queue[0]
			v.queue[0] = nil
			v.queue = v.queue[1:]
			v.bytes -= len(a.data)
			if time.Since(a.arrived) > maxAge {
				v.needIDR = true
				previous = a.sequence
				v.mu.Unlock()
				a.release()
				continue
			}
			if previous != 0 && a.sequence != previous+1 {
				v.needIDR = true
			}
			previous = a.sequence
			if v.needIDR && !a.key {
				v.mu.Unlock()
				a.release()
				continue
			}
			v.needIDR = false
			v.mu.Unlock()
			if base == 0 {
				base = a.mediaNS
			}
			elapsed := a.mediaNS - base
			// Absolute integer mapping avoids per-frame rounding drift and preserves dropped time.
			timestamp := uint32((elapsed/1000000000)*90000 + (elapsed%1000000000)*90000/1000000000)
			packets := payloader.Payload(1200, a.data)
			v.writing.Store(monotonicNS())
			failed := false
			lost := false
			for i, p := range packets {
				select {
				case <-v.done:
					failed = true
				default:
				}
				if failed {
					break
				}
				if !pacer.wait(v.done, len(p)+22, a.arrived.Add(maxAge)) {
					lost = true
					break
				}
				packet := rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: sequence, Timestamp: timestamp, Marker: i == len(packets)-1}, Payload: p}
				sequence++
				if err := v.track.WriteRTP(&packet); err != nil {
					failed = true
					break
				}
				if v.udpLoss.Load() != lastUDPLoss {
					lost = true
					break
				}
			}
			v.writing.Store(0)
			v.lastMedia.Store(monotonicNS())
			a.release()
			if lost {
				v.mu.Lock()
				v.needIDR = true
				v.mu.Unlock()
			}
			if failed {
				go v.close()
				return
			}
		}
	}
}
func (h *mediaHub) expireSessions() {
	h.mu.Lock()
	var expired []*videoSession
	for _, v := range h.sessions {
		write := v.writing.Load()
		media := v.lastMedia.Load()
		if (!v.connected.Load() && time.Since(v.created) > 15*time.Second) || (write != 0 && monotonicNS()-write > int64(time.Second)) || (v.connected.Load() && media != 0 && monotonicNS()-media > int64(3*time.Second)) {
			expired = append(expired, v)
		}
	}
	h.mu.Unlock()
	for _, v := range expired {
		go v.close()
	}
}
func (h *mediaHub) closeAll() {
	h.mu.Lock()
	h.stopping = true
	var all []*videoSession
	for _, v := range h.sessions {
		all = append(all, v)
	}
	h.mu.Unlock()
	closed := make(chan struct{})
	go func() {
		for _, v := range all { v.close() }
		close(closed)
	}()
	deadline := time.NewTimer(time.Second)
	defer deadline.Stop()
	select { case <-closed: case <-deadline.C: return }
	for _, v := range all {
		select { case <-v.finished: case <-deadline.C: return }
	}
}
func validOffer(text string) bool {
	if len(text) > 64*1024 {
		return false
	}
	var description sdp.SessionDescription
	if description.Unmarshal([]byte(text)) != nil || len(description.MediaDescriptions) != 1 {
		return false
	}
	media := description.MediaDescriptions[0]
	if media.MediaName.Media != "video" || media.MediaName.Port.Value == 0 {
		return false
	}
	_, receive := media.Attribute("recvonly")
	if !receive {
		_, receive = description.Attribute("recvonly")
	}
	if !receive {
		return false
	}
	for _, a := range media.Attributes {
		if a.Key == "sendrecv" || a.Key == "sendonly" || a.Key == "inactive" {
			return false
		}
	}
	return true
}
func (s *service) createSession(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		jsonError(w, 405, "method", "POST required")
		return
	}
	parts := strings.Split(r.URL.Path, "/")
	if len(parts) != 6 || parts[5] != "sessions" {
		jsonError(w, 404, "resource", "unknown camera")
		return
	}
	id, e := strconv.Atoi(parts[4])
	if e != nil || id < 0 || id > 3 || strconv.Itoa(id) != parts[4] || s.config.CameraMask&(1<<id) == 0 {
		jsonError(w, 404, "camera", "camera is not enabled")
		return
	}
	if s.config.Codec != "h264" {
		jsonError(w, 422, "codec", "Web video supports H.264 only")
		return
	}
	if r.Header.Get("Content-Type") != "application/json" {
		jsonError(w, 400, "body", "JSON offer required")
		return
	}
	var offer struct {
		Type string `json:"type"`
		SDP  string `json:"sdp"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 96*1024))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&offer) != nil || decoder.Decode(&struct{}{}) != io.EOF || offer.Type != "offer" || !validOffer(offer.SDP) {
		jsonError(w, 400, "offer", "one receive-only video offer required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	v := &videoSession{id: randomID(), camera: id, hub: s.hub, created: time.Now(), needIDR: true, notify: make(chan struct{}, 1), done: make(chan struct{}), finished: make(chan struct{})}
	s.hub.mu.Lock()
	if s.hub.stopping {
		s.hub.mu.Unlock()
		jsonError(w, 503, "shutdown", "application is stopping")
		return
	}
	count := 0
	for _, old := range s.hub.sessions {
		if old.camera == id {
			count++
		}
	}
	if len(s.hub.sessions) >= s.config.MaxVideo || count >= 2 {
		s.hub.mu.Unlock()
		jsonError(w, 429, "limit", "video reader limit reached")
		return
	}
	// Reserve before negotiation; independent PeerConnections do not enforce
	// hub-wide SSRC uniqueness. Closing peers retain the reservation until reaped.
	for v.ssrc == 0 {
		s.hub.nextSSRC++
		if s.hub.nextSSRC == 0 { continue }
		used := false
		for _, old := range s.hub.sessions {
			if old.ssrc == s.hub.nextSSRC { used = true; break }
		}
		if !used { v.ssrc = s.hub.nextSSRC }
	}
	s.hub.sessions[v.id] = v
	s.hub.mu.Unlock()
	success := false
	started := false
	defer func() {
		if !success {
			v.close()
		}
		if !started {
			close(v.finished)
		}
	}()
	// Pending sessions publish demand, so the source can supply the actual SPS before SDP.
	var profile string
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	for {
		profile, e = s.hub.sourceProfile(id)
		if e == nil {
			break
		}
		select {
		case <-ctx.Done():
			jsonError(w, 503, "source", "source parameter sets unavailable")
			return
		case <-v.done:
			jsonError(w, 503, "session", "session closed")
			return
		case <-ticker.C:
		}
	}
	v.profile = profile
	engine := &webrtc.MediaEngine{}
	selected, e := offeredH264(offer.SDP, profile)
	if e != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnprocessableEntity)
		_ = json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{
			"code": "negotiation", "message": e.Error(), "source_profile_level_id": profile,
		}})
		return
	}
	codec := selected.RTPCodecCapability
	if engine.RegisterCodec(selected, webrtc.RTPCodecTypeVideo) != nil {
		jsonError(w, 503, "codec", "codec unavailable")
		return
	}
	registry := &interceptor.Registry{}
	if webrtc.RegisterDefaultInterceptorsWithOptions(engine, registry, webrtc.WithNackResponderOptions(nack.ResponderSize(512))) != nil {
		jsonError(w, 503, "media", "media initialization failed")
		return
	}
	// Pion's shared UDP mux caches interfaces at construction; gather from the
	// current interfaces for each peer, for both UDP and TCP candidates.
	addresses, e := net.InterfaceAddrs()
	if e != nil {
		jsonError(w, 503, "network", "local addresses unavailable")
		return
	}
	localIPs := make(map[string]bool)
	var udpAddresses []net.Addr
	for _, address := range addresses {
		ip, _, err := net.ParseCIDR(address.String())
		if err != nil || ip.To4() == nil || (s.config.ListenAddress != "0.0.0.0" && ip.String() != s.config.ListenAddress) || (ip.IsLoopback() && s.config.ListenAddress != "127.0.0.1") { continue }
		localIPs[ip.String()] = true
		udpAddresses = append(udpAddresses, &net.UDPAddr{IP: ip, Port: s.config.ICEPort})
	}
	settings := webrtc.SettingEngine{}
	settings.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeTCP4})
	if s.hub.udpMux != nil {
		settings.SetICEUDPMux(&sessionUDPMux{UDPMux: s.hub.udpMux, addresses: udpAddresses})
	}
	settings.SetICETCPMux(s.hub.tcpMux)
	settings.SetICETimeouts(2*time.Second, 4*time.Second, time.Second)
	settings.SetIncludeLoopbackCandidate(s.config.ListenAddress == "127.0.0.1")
	settings.SetIPFilter(func(ip net.IP) bool {
		return localIPs[ip.String()]
	})
	api := webrtc.NewAPI(webrtc.WithMediaEngine(engine), webrtc.WithInterceptorRegistry(registry), webrtc.WithSettingEngine(settings))
	pc, e := api.NewPeerConnection(webrtc.Configuration{})
	if e != nil {
		jsonError(w, 503, "media", "peer initialization failed")
		return
	}
	v.mu.Lock()
	if v.closing {
		v.mu.Unlock()
		pc.Close()
		jsonError(w, 503, "session", "session closed")
		return
	}
	v.pc = pc
	v.mu.Unlock()
	track, e := webrtc.NewTrackLocalStaticRTP(codec, fmt.Sprintf("camera-%d", id), "sensor")
	if e != nil {
		jsonError(w, 503, "media", "track initialization failed")
		return
	}
	v.track = track
	transceiver, e := pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{
		Direction: webrtc.RTPTransceiverDirectionSendonly,
		SendEncodings: []webrtc.RTPEncodingParameters{{RTPCodingParameters: webrtc.RTPCodingParameters{SSRC: webrtc.SSRC(v.ssrc)}}},
	})
	if e != nil {
		jsonError(w, 503, "media", "sender initialization failed")
		return
	}
	sender := transceiver.Sender()
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		switch state {
		case webrtc.PeerConnectionStateConnected:
			v.connected.Store(true)
			v.lastMedia.Store(monotonicNS())
		case webrtc.PeerConnectionStateDisconnected, webrtc.PeerConnectionStateFailed:
			go v.close()
		}
	})
	go func() {
		for {
			packets, _, err := sender.ReadRTCP()
			if err != nil {
				return
			}
			for _, p := range packets {
				switch p.(type) {
				case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
					v.mu.Lock()
					v.needIDR = true
					v.mu.Unlock()
				}
			}
		}
	}()
	if pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: offer.SDP}) != nil {
		jsonError(w, 422, "negotiation", "H.264 source profile cannot be negotiated")
		return
	}
	transceivers := pc.GetTransceivers()
	if len(transceivers) != 1 || transceivers[0].SetCodecPreferences([]webrtc.RTPCodecParameters{selected}) != nil {
		jsonError(w, 422, "negotiation", "source codec cannot be selected")
		return
	}
	answer, e := pc.CreateAnswer(nil)
	if e != nil {
		jsonError(w, 422, "negotiation", "offer cannot be answered")
		return
	}
	gather := webrtc.GatheringCompletePromise(pc)
	if pc.SetLocalDescription(answer) != nil {
		jsonError(w, 422, "negotiation", "answer unavailable")
		return
	}
	select {
	case <-ctx.Done():
		jsonError(w, 503, "timeout", "session handshake timed out")
		return
	case <-gather:
	case <-v.done:
		jsonError(w, 503, "session", "session closed")
		return
	}
	go v.worker()
	started = true
	w.Header().Set("Location", "/api/v1/video-sessions/"+v.id)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(201)
	if json.NewEncoder(w).Encode(map[string]string{"session_id": v.id, "type": "answer", "sdp": pc.LocalDescription().SDP}) != nil {
		return
	}
	success = true
}
func (s *service) deleteSession(w http.ResponseWriter, r *http.Request) {
	if r.Method != "DELETE" {
		jsonError(w, 405, "method", "DELETE required")
		return
	}
	id := strings.TrimPrefix(r.URL.Path, "/api/v1/video-sessions/")
	if len(id) != 32 {
		jsonError(w, 404, "session", "unknown session")
		return
	}
	for _, c := range id {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			jsonError(w, 404, "session", "unknown session")
			return
		}
	}
	s.hub.mu.Lock()
	v := s.hub.sessions[id]
	s.hub.mu.Unlock()
	if v != nil {
		v.close()
	}
	w.WriteHeader(204)
}
