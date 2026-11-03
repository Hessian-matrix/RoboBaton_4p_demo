package main

import (
	"errors"
	"net"
	"net/netip"
	"sync"
	"syscall"
)

// Each send is one nonblocking syscall. No peer cancellation changes the shared
// socket deadline, and congestion cannot hold Go's socket write lock.
type udpPacketConn struct {
	*net.UDPConn
	raw syscall.RawConn
	hub *mediaHub
	mu sync.Mutex
	address syscall.SockaddrInet4
	packet []byte
	sendError error
	send func(uintptr)
}

func newUDPPacketConn(conn *net.UDPConn, hub *mediaHub) (*udpPacketConn, error) {
	raw, err := conn.SyscallConn()
	if err != nil { return nil, err }
	c := &udpPacketConn{UDPConn: conn, raw: raw, hub: hub}
	// Reuse the callback and sockaddr instead of allocating them per RTP packet.
	c.send = func(fd uintptr) {
		c.sendError = syscall.Sendto(int(fd), c.packet, syscall.MSG_DONTWAIT, &c.address)
	}
	return c, nil
}

func (c *udpPacketConn) ReadFromAddrPort(b []byte) (int, netip.AddrPort, error) {
	return c.ReadFromUDPAddrPort(b)
}
func (c *udpPacketConn) WriteTo(b []byte, address net.Addr) (int, error) {
	udp, ok := address.(*net.UDPAddr)
	if !ok { return 0, syscall.EINVAL }
	return c.WriteToAddrPort(b, udp.AddrPort())
}
func (c *udpPacketConn) WriteToAddrPort(b []byte, address netip.AddrPort) (int, error) {
	if !address.IsValid() || !address.Addr().Unmap().Is4() { return 0, syscall.EINVAL }
	c.mu.Lock()
	c.address.Addr = address.Addr().Unmap().As4()
	c.address.Port = int(address.Port())
	c.packet = b
	c.sendError = nil
	err := c.raw.Control(c.send)
	if err == nil { err = c.sendError }
	c.packet = nil
	c.mu.Unlock()
	if errors.Is(err, syscall.EAGAIN) || errors.Is(err, syscall.ENOBUFS) || errors.Is(err, syscall.EINTR) {
		// UDP loss is recoverable. The clear SRTP header identifies only the
		// affected sender; STUN/DTLS retain their protocol retransmission policy.
		if len(b) >= 12 && b[0]&0xc0 == 0x80 && (b[1] < 192 || b[1] > 223) {
			ssrc := be.Uint32(b[8:12])
			c.hub.mu.Lock()
			for _, v := range c.hub.sessions {
				if v.ssrc == ssrc { v.udpLoss.Add(1) }
			}
			c.hub.mu.Unlock()
		}
		return len(b), nil
	}
	if err != nil { return 0, err }
	return len(b), nil
}
