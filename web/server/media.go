package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"github.com/pion/ice/v4"
	"math"
	"net"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	videoMagic   = 0x53575631
	imuMagic     = 0x53574932
	controlMagic = 0x53574331
	statusMagic  = 0x53575331
	maxAU        = 2 * 1024 * 1024
	chunkBytes   = 60 * 1024
	maxAge       = 250 * time.Millisecond
	maxPayload   = 32 * 1024 * 1024
)

var be = binary.BigEndian

var clockEpoch = time.Now()
func monotonicNS() int64 { return time.Since(clockEpoch).Nanoseconds() + 1 }

type accessUnit struct {
	data                     []byte
	refs                     atomic.Int32
	hub                      *mediaHub
	sequence, mediaNS, epoch uint64
	arrived                  time.Time
	key                      bool
	profile                  string
}

func (a *accessUnit) release() {
	if a.refs.Add(-1) == 0 {
		a.hub.release(len(a.data))
	}
}

type assembly struct {
	data                     []byte
	sequence, mediaNS, epoch uint64
	next, count              uint16
	arrived                  time.Time
}
type cameraSource struct {
	mu              sync.Mutex
	partial         *assembly
	sequence, epoch uint64
	needIDR         bool
	sps, pps        []byte
	profile         string
	last            time.Time
	drops           uint64
}
type mediaHub struct {
	cameras  [4]cameraSource
	used     atomic.Int64
	demand   atomic.Uint32
	mu       sync.Mutex
	sessions map[string]*videoSession
	config   config
	stopping bool
	udpMux   ice.UDPMux
	tcpMux   ice.TCPMux
	nextSSRC uint32
}

func newHub(c config) *mediaHub {
	h := &mediaHub{config: c, sessions: make(map[string]*videoSession)}
	var seed [4]byte
	if _, err := rand.Read(seed[:]); err != nil { panic(err) }
	h.nextSSRC = be.Uint32(seed[:])
	for i := range h.cameras {
		h.cameras[i].needIDR = true
	}
	return h
}
func (h *mediaHub) reserve(n int) bool {
	for {
		v := h.used.Load()
		if v+int64(n) > maxPayload {
			return false
		}
		if h.used.CompareAndSwap(v, v+int64(n)) {
			return true
		}
	}
}
func (h *mediaHub) release(n int) { h.used.Add(-int64(n)) }
func validHeader(b []byte, magic uint32, length int, generation uint64) bool {
	return len(b) >= length && be.Uint32(b) == magic && be.Uint16(b[4:]) == 1 && int(be.Uint16(b[6:])) == length && be.Uint64(b[8:]) == generation
}
func (h *mediaHub) videoLoop(c *net.UnixConn) {
	buffer := make([]byte, 80+chunkBytes)
	for {
		n, _, flags, _, err := c.ReadMsgUnix(buffer, nil)
		if err != nil {
			return
		}
		b := buffer[:n]
		if flags&syscall.MSG_TRUNC != 0 || !validHeader(b, videoMagic, 80, h.config.Generation) {
			continue
		}
		id := be.Uint32(b[24:])
		if id > 3 || h.config.CameraMask&(1<<id) == 0 {
			continue
		}
		h.ingest(int(id), b)
	}
}
func (h *mediaHub) ingest(id int, b []byte) {
	c := &h.cameras[id]
	c.mu.Lock()
	defer c.mu.Unlock()
	drop := func() {
		if c.partial != nil {
			h.release(len(c.partial.data))
			c.partial = nil
		}
		c.needIDR = true
		c.drops++
	}
	total := int(be.Uint32(b[36:]))
	seq := be.Uint64(b[40:])
	epoch := be.Uint64(b[16:])
	offset := int(be.Uint32(b[64:]))
	index := be.Uint16(b[68:])
	count := be.Uint16(b[70:])
	size := int(be.Uint32(b[72:]))
	if epoch == 0 || epoch < c.epoch || h.demand.Load()&(1<<id) == 0 {
		return
	}
	if epoch != c.epoch {
		previous := c.epoch
		if c.partial != nil {
			h.release(len(c.partial.data))
			c.partial = nil
		}
		c.sequence = 0
		c.epoch = epoch
		c.needIDR = true
		c.sps = nil
		c.pps = nil
		c.profile = ""
		c.last = time.Time{}
		if previous != 0 {
			h.closeCamera(id)
		}
	}
	if be.Uint32(b[28:]) != 1 || total <= 0 || total > maxAU || size <= 0 || size > chunkBytes ||
		size != len(b)-80 || offset != int(index)*chunkBytes || count != uint16((total+chunkBytes-1)/chunkBytes) ||
		index >= count || offset > total || size != min(chunkBytes, total-offset) || be.Uint32(b[76:]) != 0 {
		drop()
		return
	}
	now := time.Now()
	if c.partial != nil && now.Sub(c.partial.arrived) > maxAge {
		drop()
	}
	if index == 0 {
		if c.partial != nil {
			drop()
		}
		if c.sequence != 0 && seq != c.sequence+1 {
			c.needIDR = true
			c.drops++
		}
		if seq <= c.sequence {
			drop()
			return
		}
		if !h.reserve(total) {
			drop()
			return
		}
		c.partial = &assembly{data: make([]byte, total), sequence: seq, mediaNS: be.Uint64(b[56:]), epoch: epoch, count: count, arrived: now}
	}
	a := c.partial
	if a == nil || a.sequence != seq || a.epoch != epoch || a.mediaNS != be.Uint64(b[56:]) || a.next != index || len(a.data) != total || a.count != count {
		drop()
		return
	}
	copy(a.data[offset:], b[80:])
	a.next++
	if a.next != count {
		return
	}
	c.partial = nil
	c.sequence = seq
	reserved := len(a.data)
	transferred := false
	defer func() {
		if !transferred {
			h.release(reserved)
		}
	}()
	key := false
	var sps, pps []byte
	for _, nal := range annexNals(a.data) {
		switch nal[0] & 31 {
		case 7:
			sps = nal
		case 8:
			pps = nal
		case 5:
			key = true
		}
	}
	// Configuration changes close sessions; an old SDP is never relabelled as a new codec.
	changed := (sps != nil && c.sps != nil && !bytes.Equal(sps, c.sps)) || (pps != nil && c.pps != nil && !bytes.Equal(pps, c.pps))
	if sps != nil {
		if len(sps) < 4 || len(sps) > 4096 {
			drop()
			return
		}
		c.sps = append(c.sps[:0], sps...)
		c.profile = hex.EncodeToString(sps[1:4])
	}
	if pps != nil {
		if len(pps) > 4096 {
			drop()
			return
		}
		c.pps = append(c.pps[:0], pps...)
	}
	if changed {
		c.needIDR = true
		h.closeCamera(id)
	}
	if c.needIDR && (!key || len(c.sps) == 0 || len(c.pps) == 0) {
		return
	}
	if key && (sps == nil || pps == nil) {
		n := len(a.data) + len(c.sps) + len(c.pps) + 8
		if n > maxAU || !h.reserve(n) {
			drop()
			return
		}
		combined := make([]byte, 0, n)
		combined = append(combined, 0, 0, 0, 1)
		combined = append(combined, c.sps...)
		combined = append(combined, 0, 0, 0, 1)
		combined = append(combined, c.pps...)
		combined = append(combined, a.data...)
		a.data = combined
		h.release(reserved)
		reserved = n
	}
	c.needIDR = false
	c.last = now
	unit := &accessUnit{data: a.data, hub: h, sequence: seq, mediaNS: a.mediaNS, epoch: epoch, arrived: a.arrived, key: key, profile: c.profile}
	unit.refs.Store(1)
	transferred = true
	defer unit.release()
	h.mu.Lock()
	for _, s := range h.sessions {
		if s.camera == id {
			s.enqueue(unit)
		}
	}
	h.mu.Unlock()
}
func annexNals(b []byte) [][]byte {
	var result [][]byte
	start := -1
	for i := 0; i+3 <= len(b); {
		prefix := 0
		if b[i] == 0 && b[i+1] == 0 {
			if b[i+2] == 1 {
				prefix = 3
			} else if i+4 <= len(b) && b[i+2] == 0 && b[i+3] == 1 {
				prefix = 4
			}
		}
		if prefix != 0 {
			if start >= 0 && i > start {
				result = append(result, b[start:i])
			}
			start = i + prefix
			i += prefix
		} else {
			i++
		}
	}
	if start >= 0 && start < len(b) {
		result = append(result, b[start:])
	}
	return result
}
func (h *mediaHub) closeCamera(id int) {
	h.mu.Lock()
	var list []*videoSession
	for _, s := range h.sessions {
		if s.camera == id {
			list = append(list, s)
		}
	}
	h.mu.Unlock()
	for _, s := range list {
		if s.revoke() { go s.finishClose() }
	}
}
func (h *mediaHub) expireAssemblies() {
	for i := range h.cameras {
		c := &h.cameras[i]
		c.mu.Lock()
		if c.partial != nil && time.Since(c.partial.arrived) > maxAge {
			h.release(len(c.partial.data))
			c.partial = nil
			c.needIDR = true
			c.drops++
		}
		c.mu.Unlock()
	}
}
func (h *mediaHub) sourceProfile(id int) (string, error) {
	c := &h.cameras[id]
	c.mu.Lock()
	defer c.mu.Unlock()
	if h.config.Codec != "h264" {
		return "", errors.New("unsupported codec")
	}
	if c.profile == "" || time.Since(c.last) > 2*time.Second {
		return "", errors.New("waiting for source parameter sets")
	}
	return c.profile, nil
}
func finite(v float64) any {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return nil
	}
	return v
}
