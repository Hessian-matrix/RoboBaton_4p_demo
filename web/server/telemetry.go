package main

import (
	"encoding/json"
	"fmt"
	"math"
	"net"
	"net/http"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type telemetryHub struct {
	config      config
	clients     atomic.Int32
	mu          sync.Mutex
	subscribers map[*imuClient]bool
	last        time.Time
}
type imuClient struct {
	queue      chan []byte
	overwrites atomic.Uint64
}

func newTelemetry(c config) *telemetryHub {
	return &telemetryHub{config: c, subscribers: make(map[*imuClient]bool)}
}
func (h *telemetryHub) state() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.last.IsZero() {
		return "idle"
	}
	if time.Since(h.last) > 500*time.Millisecond {
		return "stale"
	}
	return "flowing"
}
func (h *telemetryHub) readLoop(c *net.UnixConn) {
	buffer := make([]byte, 257)
	for {
		n, _, flags, _, err := c.ReadMsgUnix(buffer, nil)
		if err != nil {
			return
		}
		b := buffer[:n]
		if n != 256 || flags&syscall.MSG_TRUNC != 0 || !validHeader(b, imuMagic, 256, h.config.Generation) {
			continue
		}
		h.mu.Lock()
		h.last = time.Now()
		if len(h.subscribers) == 0 {
			h.mu.Unlock()
			continue
		}
		accel := make([]any, 3)
		gyro := make([]any, 3)
		attitudeAccel := make([]any, 3)
		attitudeGyro := make([]any, 3)
		for i := range 3 {
			accel[i] = finite(math.Float64frombits(be.Uint64(b[48+8*i:])))
			gyro[i] = finite(math.Float64frombits(be.Uint64(b[72+8*i:])))
			attitudeAccel[i] = finite(math.Float64frombits(be.Uint64(b[128+8*i:])))
			attitudeGyro[i] = finite(math.Float64frombits(be.Uint64(b[152+8*i:])))
		}
		mode := "host"
		var uncertainty any
		if be.Uint32(b[108:]) == 1 {
			mode = "tmst"
			uncertainty = be.Uint32(b[96:])
		}
		message := map[string]any{
			"schema_version": 2, "session_id": strconv.FormatUint(h.config.Generation, 10),
			"sample_sequence": strconv.FormatUint(be.Uint64(b[16:]), 10),
			"sample_realtime_ns": strconv.FormatUint(be.Uint64(b[24:]), 10),
			"observation_monotonic_ns": strconv.FormatUint(be.Uint64(b[32:]), 10),
			"temperature_c": finite(math.Float64frombits(be.Uint64(b[40:]))),
			"accel_mps2": accel, "gyro_rps": gyro,
			"source_rate_hz": h.config.SourceRate, "telemetry_limit_hz": h.config.TelemetryHz,
			"timestamp_uncertainty_ns": uncertainty, "timestamp_mode": mode,
			"source_generation": be.Uint32(b[100:]), "burst_flags": be.Uint32(b[104:]),
			"evidence_id": strconv.FormatUint(be.Uint64(b[120:]), 10),
			"telemetry_counters": map[string]string{"ipc_send_drops": strconv.FormatUint(be.Uint64(b[112:]), 10)},
			"sample_monotonic_ns": strconv.FormatUint(be.Uint64(b[176:]), 10),
			"sample_raw_ns": strconv.FormatUint(be.Uint64(b[184:]), 10),
			"burst_complete_raw_ns": strconv.FormatUint(be.Uint64(b[192:]), 10),
			"estimated_period_ns": strconv.FormatUint(be.Uint64(b[200:]), 10),
			"bridge_monotonic_ns": strconv.FormatUint(be.Uint64(b[208:]), 10),
			"bridge_raw_ns": strconv.FormatUint(be.Uint64(b[216:]), 10),
			"bridge_realtime_ns": strconv.FormatUint(be.Uint64(b[224:]), 10),
			"bridge_uncertainty_ns": strconv.FormatUint(be.Uint64(b[232:]), 10),
			"timestamp_source": be.Uint32(b[240:]), "packet_flags": be.Uint32(b[244:]),
			"fifo_tmst": be.Uint32(b[248:]),
		}
		message["attitude_accel_mps2"] = attitudeAccel
		message["attitude_gyro_rps"] = attitudeGyro
		body, e := json.Marshal(message)
		if e == nil {
			for client := range h.subscribers {
				select {
				case client.queue <- body:
				default:
					select {
					case <-client.queue:
						client.overwrites.Add(1)
					default:
					}
					select {
					case client.queue <- body:
					default:
					}
				}
			}
		}
		h.mu.Unlock()
	}
}
func (h *telemetryHub) serve(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		jsonError(w, 405, "method", "GET required")
		return
	}
	if h.clients.Add(1) > int32(h.config.MaxIMU) {
		h.clients.Add(-1)
		jsonError(w, 429, "limit", "IMU reader limit reached")
		return
	}
	defer h.clients.Add(-1)
	client := &imuClient{queue: make(chan []byte, 1)}
	h.mu.Lock()
	h.subscribers[client] = true
	h.mu.Unlock()
	defer func() { h.mu.Lock(); delete(h.subscribers, client); h.mu.Unlock() }()
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Accel-Buffering", "no")
	controller := http.NewResponseController(w)
	write := func(event string, body []byte) bool {
		if controller.SetWriteDeadline(time.Now().Add(time.Second)) != nil {
			return false
		}
		if _, err := fmt.Fprintf(w, "event: %s\ndata: %s\n\n", event, body); err != nil {
			return false
		}
		return controller.Flush() == nil
	}
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	if !write("status", []byte(`{"state":"waiting"}`)) {
		return
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case body := <-client.queue:
			if !write("imu", body) {
				return
			}
		case <-tick.C:
			body, _ := json.Marshal(map[string]string{"state": h.state(), "client_overwrites": strconv.FormatUint(client.overwrites.Load(), 10)})
			if !write("status", body) {
				return
			}
		}
	}
}
