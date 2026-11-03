package main

import (
	"bytes"
	"crypto/rand"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"sync"
	"time"
)

const (
	recordingRequestMagic = 0x53575231
	recordingStatusMagic  = 0x53575232
	recordingMaxDatagram  = 4096
	recordingFreshness    = time.Second
	recordingAckTimeout   = 3 * time.Second
)

// Decimal strings preserve the full producer-owned uint64 range in browsers.
// Output paths are producer-selected; HTTP requests never carry a path.
type recordingStatus struct {
	RequestID       string `json:"request_id"`
	ReplyCode       int    `json:"reply_code"`
	SessionID       string `json:"session_id"`
	State           string `json:"state"`
	Format          string `json:"format"`
	Storage         string `json:"storage"`
	Path            string `json:"path"`
	Error           string `json:"error"`
	Outcome         string `json:"outcome"`
	DataComplete    bool   `json:"data_complete"`
	CleanupComplete bool   `json:"cleanup_complete"`
	ElapsedMS       string `json:"elapsed_ms"`
	BytesWritten    string `json:"bytes_written"`
	DroppedGroups   string `json:"dropped_groups"`
	FreeBytes       string `json:"free_bytes"`
	DroppedFrames   string `json:"dropped_frames,omitempty"`
	CanStart        bool   `json:"can_start"`
	MP4Available    bool   `json:"mp4_available"`
	HealthScope     string `json:"health_scope,omitempty"`
}

type recordingMutation struct {
	requestID string
	done      chan struct{}
	status    recordingStatus
	at           time.Time
	acknowledged bool
	detached     bool
}

type recordingCache struct {
	mu      sync.Mutex
	status  recordingStatus
	at      time.Time
	pending *recordingMutation
}

func decimalUint64(value string) (uint64, bool) {
	n, err := strconv.ParseUint(value, 10, 64)
	return n, err == nil && strconv.FormatUint(n, 10) == value
}

func validRecordingStatus(status recordingStatus) bool {
	for _, value := range []string{status.RequestID, status.SessionID, status.ElapsedMS, status.BytesWritten, status.DroppedGroups, status.FreeBytes} {
		if _, ok := decimalUint64(value); !ok { return false }
	}
	if status.DroppedFrames != "" { if _, ok := decimalUint64(status.DroppedFrames); !ok { return false } }
	switch status.ReplyCode {
	case 200, 202, 409, 400, 503:
	default:
		return false
	}
	switch status.State {
	case "idle", "starting", "recording", "stopping", "complete", "partial", "failed":
	default:
		return false
	}
	return (status.Format == "" || status.Format == "rosbag" || status.Format == "mp4") &&
		(status.Storage == "runtime" || status.Storage == "tf") &&
		len(status.Path) <= 2048 && len(status.Error) <= 1024 &&
		len(status.Outcome) <= 128 && len(status.HealthScope) <= 128
}

func (s *service) receiveRecordingStatus(packet []byte) {
	if len(packet) <= 16 || len(packet) > recordingMaxDatagram || !validHeader(packet, recordingStatusMagic, len(packet), s.config.Generation) { return }
	var status recordingStatus
	decoder := json.NewDecoder(bytes.NewReader(packet[16:]))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&status) != nil || decoder.Decode(&struct{}{}) != io.EOF || !validRecordingStatus(status) { return }
	s.recording.mu.Lock()
	defer s.recording.mu.Unlock()
	s.recording.status, s.recording.at = status, time.Now()
	if pending := s.recording.pending; pending != nil && !pending.acknowledged && pending.requestID == status.RequestID {
		pending.status, pending.acknowledged, pending.at = status, true, time.Now()
		if pending.detached { s.recording.pending = nil }
		close(pending.done)
	}
}

func (s *service) recordingFreshLocked() bool {
	return s.control != nil && !s.recording.at.IsZero() && time.Since(s.recording.at) <= recordingFreshness
}

// Both demand heartbeats and mutations share the connection's write deadline.
func (s *service) writeControl(packet []byte) error {
	s.controlWriteMu.Lock()
	defer s.controlWriteMu.Unlock()
	if s.control == nil { return errors.New("control unavailable") }
	if err := s.control.SetWriteDeadline(time.Now().Add(20*time.Millisecond)); err != nil { return err }
	_, err := s.control.Write(packet)
	return err
}

type recordingRequest struct {
	Action            string          `json:"action"`
	ExpectedSessionID string          `json:"expected_session_id"`
	Format            json.RawMessage `json:"format"`
	Storage           json.RawMessage `json:"storage"`
	FrameSkip         json.RawMessage `json:"frame_skip"`
	DropMinInterval   json.RawMessage `json:"drop_min_interval"`
}

func decodeRecordingRequest(w http.ResponseWriter, r *http.Request) (recordingRequest, [64]byte, bool) {
	var request recordingRequest
	var packet [64]byte
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&request) != nil || decoder.Decode(&struct{}{}) != io.EOF {
		jsonError(w, 400, "body", "one strict recording JSON object required (maximum 1024 bytes)")
		return request, packet, false
	}
	session, ok := decimalUint64(request.ExpectedSessionID)
	if !ok || (request.Action != "start" && request.Action != "stop") {
		jsonError(w, 400, "recording", "action must be start or stop and expected_session_id must be a decimal uint64 string")
		return request, packet, false
	}
	be.PutUint64(packet[48:], session)
	if request.Action == "stop" {
		if request.Format != nil || request.Storage != nil || request.FrameSkip != nil || request.DropMinInterval != nil {
			jsonError(w, 400, "recording", "stop accepts only action and expected_session_id")
			return request, packet, false
		}
		be.PutUint32(packet[16:], 2)
		return request, packet, true
	}
	var format, storage string
	var frameSkip, dropMinInterval uint32
	if json.Unmarshal(request.Format, &format) != nil || json.Unmarshal(request.Storage, &storage) != nil ||
		len(request.FrameSkip) == 0 || bytes.Equal(request.FrameSkip, []byte("null")) || json.Unmarshal(request.FrameSkip, &frameSkip) != nil ||
		len(request.DropMinInterval) == 0 || bytes.Equal(request.DropMinInterval, []byte("null")) || json.Unmarshal(request.DropMinInterval, &dropMinInterval) != nil ||
		(format != "rosbag" && format != "mp4") || (storage != "runtime" && storage != "tf") ||
		frameSkip > 1 || dropMinInterval < 2 || (frameSkip == 1 && (format == "mp4" || dropMinInterval != 2)) {
		jsonError(w, 400, "recording", "start requires fixed format/storage, frame_skip 0 or 1 (MP4: 0), and uint32 drop_min_interval >=2 (skip: 2)")
		return request, packet, false
	}
	be.PutUint32(packet[16:], 1)
	mode := uint32(1)
	if format == "mp4" { mode = 2 }
	be.PutUint32(packet[20:], mode)
	if storage == "tf" { be.PutUint32(packet[24:], 1) }
	be.PutUint32(packet[28:], frameSkip)
	be.PutUint32(packet[32:], dropMinInterval)
	return request, packet, true
}

func recordingJSON(w http.ResponseWriter, code int, status recordingStatus) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(status)
}

func (s *service) serveRecording(w http.ResponseWriter, r *http.Request) {
	if r.Method == "GET" {
		s.recording.mu.Lock()
		status, fresh := s.recording.status, s.recordingFreshLocked()
		age := time.Since(s.recording.at)
		s.recording.mu.Unlock()
		if !fresh { jsonError(w, 503, "recording_unavailable", "recording status unavailable or stale; controls disabled"); return }
		w.Header().Set("X-Recording-Status-Age-Ms", strconv.FormatInt(age.Milliseconds(), 10))
		recordingJSON(w, 200, status)
		return
	}
	if r.Method != "POST" { jsonError(w, 405, "method", "GET or POST required"); return }
	if r.Header.Get("Origin") != "http://"+r.Host {
		jsonError(w, 400, "origin", "exact same Origin required for recording control")
		return
	}
	if r.Header.Get("Content-Type") != "application/json" {
		jsonError(w, 400, "body", "application/json required")
		return
	}
	request, packet, ok := decodeRecordingRequest(w, r)
	if !ok { return }
	s.recording.mu.Lock()
	if !s.recordingFreshLocked() {
		s.recording.mu.Unlock()
		jsonError(w, 503, "recording_unavailable", "recording status unavailable or stale; controls disabled")
		return
	}
	if s.recording.pending != nil {
		s.recording.mu.Unlock()
		jsonError(w, 409, "recording_pending", "a recording request is awaiting acknowledgement; query state, do not retry")
		return
	}
	status := s.recording.status
	if request.ExpectedSessionID != status.SessionID {
		s.recording.mu.Unlock()
		jsonError(w, 409, "recording_session", "recording session changed; query current state before acting")
		return
	}
	if (request.Action == "start" && !status.CanStart) || (request.Action == "stop" && status.State != "recording") {
		s.recording.mu.Unlock()
		jsonError(w, 409, "recording_state", "recording action conflicts with current state; query current state")
		return
	}
	if request.Action == "start" && be.Uint32(packet[20:]) == 2 && !status.MP4Available {
		s.recording.mu.Unlock()
		jsonError(w, 400, "recording_format", "MP4 recording unavailable; requires H.264 four-camera support")
		return
	}
	// Retain an unacknowledged mutation after HTTP timeout/disconnect. A second
	// command is unsafe until the producer's repeated status acknowledges it.
	var random [8]byte
	var requestID uint64
	for requestID == 0 || strconv.FormatUint(requestID, 10) == status.RequestID {
		if _, err := rand.Read(random[:]); err != nil {
			s.recording.mu.Unlock()
			jsonError(w, 503, "recording_unavailable", "request identifier unavailable")
			return
		}
		requestID = be.Uint64(random[:])
	}
	pending := &recordingMutation{requestID: strconv.FormatUint(requestID, 10), done: make(chan struct{})}
	s.recording.pending = pending
	s.recording.mu.Unlock()
	defer func() {
		s.recording.mu.Lock()
		pending.detached = true
		if pending.acknowledged && s.recording.pending == pending { s.recording.pending = nil }
		s.recording.mu.Unlock()
	}()
	be.PutUint32(packet[:], recordingRequestMagic)
	be.PutUint16(packet[4:], 1)
	be.PutUint16(packet[6:], 64)
	be.PutUint64(packet[8:], s.config.Generation)
	be.PutUint64(packet[40:], requestID)
	if s.writeControl(packet[:]) != nil {
		jsonError(w, 503, "recording_uncertain", "recording command delivery uncertain; query /api/v1/recording before acting; do not blindly retry")
		return
	}
	timer := time.NewTimer(recordingAckTimeout)
	defer timer.Stop()
	select {
	case <-pending.done:
		s.recording.mu.Lock()
		fresh := s.recordingFreshLocked() && time.Since(pending.at) <= recordingFreshness
		age := time.Since(pending.at)
		s.recording.mu.Unlock()
		if !fresh {
			jsonError(w, 503, "recording_uncertain", "recording acknowledgement is stale; query /api/v1/recording; do not blindly retry")
			return
		}
		w.Header().Set("X-Recording-Status-Age-Ms", strconv.FormatInt(age.Milliseconds(), 10))
		if pending.status.ReplyCode == 200 || pending.status.ReplyCode == 202 {
			recordingJSON(w, pending.status.ReplyCode, pending.status)
		} else {
			message := pending.status.Error
			if message == "" { message = "recording command rejected; query current state" }
			jsonError(w, pending.status.ReplyCode, "recording_rejected", message)
		}
	case <-timer.C:
		jsonError(w, 503, "recording_uncertain", "recording acknowledgement timed out; state uncertain; query /api/v1/recording; do not blindly retry")
	case <-r.Context().Done():
		// Browser departure never stops or cancels the producer's recording.
		return
	}
}
