use crate::{ApiError, Config, negotiation};
use mio::{Events, Interest, Poll, Token, Waker, unix::SourceFd};
use serde::Serialize;
use std::{
    collections::VecDeque,
    io::{self, Read, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    os::{fd::AsRawFd, unix::net::UnixDatagram},
    sync::{Arc, Mutex, atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering}, mpsc::{self, Receiver, SyncSender}},
    thread::JoinHandle,
    time::{Duration, Instant},
};
use str0m::{Candidate, Event, IceConnectionState, Input, Output, Rtc, RtcConfig,
    format::{Codec, CodecSpec, FormatParams, PayloadParams},
    media::{Frequency, Mid, Pt},
    net::{Protocol, Receive, TcpType, Transmit},
    rtp::{RawPacket, RtpWrite, Ssrc, rtcp::Rtcp},
};
use tokio::sync::{oneshot, watch};

const MAX_AU: usize = 2 * 1024 * 1024;
const CHUNK: usize = 60 * 1024;
const MAX_PAYLOAD: usize = 32 * 1024 * 1024;
const MAX_AGE: Duration = Duration::from_millis(250);
const HANDSHAKE: Duration = Duration::from_secs(15);
const SOURCE_FRESH: Duration = Duration::from_secs(2);
const SESSION_BYTES: usize = 4 * 1024 * 1024;
const MAX_TCP: usize = 32;
const MAX_REMOTE_ENDPOINTS: usize = 64;
const TCP_BYTES: usize = 64 * 1024;
const RTP_PAYLOAD: usize = 1200;
// str0m's cache and the RFC4571 output queue are independently bounded and
// reserved even when they retain packet Arcs after the source frame is gone.
const TRANSPORT_RESERVE: usize = 512 * 1400 + TCP_BYTES;
const VIDEO: Token = Token(0);
const UDP: Token = Token(1);
const LISTENER: Token = Token(2);
const COMMAND: Token = Token(3);
const TCP_TOKEN: usize = 4;

#[derive(Serialize)]
pub struct SessionAnswer {
    pub session_id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub sdp: String,
}

#[derive(Clone, Serialize)]
pub struct CameraStatus {
    pub camera_id: usize,
    pub web: &'static str,
    pub ipc_drops: u64,
}

#[derive(Clone)]
pub struct MediaHandle { inner: Arc<HandleInner> }
struct HandleInner {
    shared: Arc<Shared>,
    commands: SyncSender<Create>,
    waker: Arc<Waker>,
    thread: Mutex<Option<JoinHandle<()>>>,
}
struct Shared {
    stop: AtomicBool,
    demand: AtomicU32,
    state: Mutex<Snapshot>,
    finished: watch::Sender<bool>,
}
struct Snapshot {
    cameras: [CameraStatus; 4],
    ids: [Option<String>; 8],
    deleted: Vec<String>,
}
struct Create {
    camera: usize,
    offer: String,
    reply: oneshot::Sender<Result<SessionAnswer, ApiError>>,
    cancelled: Arc<AtomicBool>,
}
struct CancelCreate { cancelled: Arc<AtomicBool>, waker: Arc<Waker>, armed: bool }
impl Drop for CancelCreate {
    fn drop(&mut self) {
        if self.armed { self.cancelled.store(true, Ordering::Release); let _ = self.waker.wake(); }
    }
}
impl Drop for HandleInner {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Release);
        let _ = self.waker.wake();
    }
}
impl MediaHandle {
    pub fn spawn(config: Arc<Config>, video: UnixDatagram, udp: UdpSocket, tcp: TcpListener) -> io::Result<Self> {
        video.set_nonblocking(true)?;
        udp.set_nonblocking(true)?;
        tcp.set_nonblocking(true)?;
        let enabled: libc::c_int = 1;
        if unsafe { libc::setsockopt(udp.as_raw_fd(), libc::IPPROTO_IP, libc::IP_PKTINFO, (&enabled as *const libc::c_int).cast(), std::mem::size_of_val(&enabled) as _) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let poll = Poll::new()?;
        for (fd, token) in [(video.as_raw_fd(), VIDEO), (udp.as_raw_fd(), UDP), (tcp.as_raw_fd(), LISTENER)] {
            poll.registry().register(&mut SourceFd(&fd), token, Interest::READABLE)?;
        }
        let waker = Arc::new(Waker::new(poll.registry(), COMMAND)?);
        let (finished, _) = watch::channel(false);
        let shared = Arc::new(Shared {
            stop: AtomicBool::new(false), demand: AtomicU32::new(0), finished,
            state: Mutex::new(Snapshot {
                cameras: std::array::from_fn(|i| CameraStatus { camera_id: i, web: initial_state(&config, i), ipc_drops: 0 }),
                ids: std::array::from_fn(|_| None), deleted: Vec::with_capacity(8),
            }),
        });
        let (commands, receiver) = mpsc::sync_channel(32);
        let reactor = Reactor {
            config, video, udp, listener: tcp, poll, commands: receiver, shared: shared.clone(),
            sources: std::array::from_fn(|_| Source::default()),
            sessions: std::array::from_fn(|_| None), tcp: std::array::from_fn(|_| None),
            budget: Arc::new(AtomicUsize::new(0)), next_ssrc: rand::random(),
        };
        let thread = std::thread::Builder::new().name("web-media".into()).spawn(move || reactor.run())?;
        Ok(Self { inner: Arc::new(HandleInner { shared, commands, waker, thread: Mutex::new(Some(thread)) }) })
    }
    pub async fn create(&self, camera: usize, offer: String) -> Result<SessionAnswer, ApiError> {
        if self.inner.shared.stop.load(Ordering::Acquire) { return Err(stopping()); }
        let (reply, result) = oneshot::channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        let mut cancel = CancelCreate { cancelled: cancelled.clone(), waker: self.inner.waker.clone(), armed: true };
        self.inner.commands.try_send(Create { camera, offer, reply, cancelled }).map_err(|e| match e {
            mpsc::TrySendError::Full(_) => ApiError::new(429, "limit", "video reader limit reached"),
            mpsc::TrySendError::Disconnected(_) => stopping(),
        })?;
        let _ = self.inner.waker.wake();
        let result = result.await.unwrap_or_else(|_| Err(stopping()));
        cancel.armed = false;
        result
    }
    pub fn delete(&self, id: &str) {
        let mut snapshot = self.inner.shared.state.lock().unwrap();
        if snapshot.ids.iter().flatten().any(|v| v == id) && !snapshot.deleted.iter().any(|v| v == id) {
            snapshot.deleted.push(id.to_owned());
            drop(snapshot);
            let _ = self.inner.waker.wake();
        }
    }
    pub fn demand(&self) -> u32 { self.inner.shared.demand.load(Ordering::Acquire) & 15 }
    pub fn cameras(&self) -> [CameraStatus; 4] { self.inner.shared.state.lock().unwrap().cameras.clone() }
    pub async fn shutdown(&self) {
        let mut finished = self.inner.shared.finished.subscribe();
        self.inner.shared.stop.store(true, Ordering::Release);
        let _ = self.inner.waker.wake();
        while !*finished.borrow_and_update() {
            if finished.changed().await.is_err() { break; }
        }
        let thread = self.inner.thread.lock().unwrap().take();
        if let Some(thread) = thread { let _ = tokio::task::spawn_blocking(move || thread.join()).await; }
    }
}
fn stopping() -> ApiError { ApiError::new(503, "shutdown", "application is stopping") }
fn initial_state(c: &Config, camera: usize) -> &'static str {
    if !c.web_enabled || c.camera_mask & (1 << camera) == 0 { "disabled" } else if c.codec != "h264" { "unsupported" } else { "idle" }
}

struct Payload { bytes: Vec<u8>, budget: Arc<AtomicUsize>, reserved: usize }
impl Drop for Payload {
    fn drop(&mut self) { self.budget.fetch_sub(self.reserved, Ordering::Relaxed); }
}
fn reserve(budget: &Arc<AtomicUsize>, n: usize) -> bool {
    budget.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| old.checked_add(n).filter(|v| *v <= MAX_PAYLOAD)).is_ok()
}
struct Frame {
    packets: Box<[Arc<[u8]>]>,
    budget: Arc<AtomicUsize>,
    reserved: usize,
    bytes: usize,
    sequence: u64,
    media_ns: u64,
    arrived: Instant,
    key: bool,
}
impl Drop for Frame {
    fn drop(&mut self) { self.budget.fetch_sub(self.reserved, Ordering::Relaxed); }
}
struct Assembly {
    payload: Payload, sequence: u64, media_ns: u64, source_ns: u64,
    flags: u32, epoch: u64, next: u16, count: u16, arrived: Instant,
}
struct Source {
    partial: Option<Assembly>, sequence: u64, media_ns: u64, epoch: u64,
    need_idr: bool, sps: Vec<u8>, pps: Vec<u8>, profile: String,
    last: Option<Instant>, drops: u64,
}
impl Default for Source {
    fn default() -> Self {
        Self { partial: None, sequence: 0, media_ns: 0, epoch: 0, need_idr: true, sps: Vec::new(), pps: Vec::new(), profile: String::new(), last: None, drops: 0 }
    }
}
impl Source {
    fn lost(&mut self) {
        self.partial = None;
        self.need_idr = true;
        self.drops = self.drops.saturating_add(1);
    }
}
struct Sending { frame: Arc<Frame>, packet: usize, timestamp: u32, failed: bool, remaining: usize }
struct Session {
    id: String, camera: usize, created: Instant,
    reply: Option<oneshot::Sender<Result<SessionAnswer, ApiError>>>, offer: Option<String>,
    cancelled: Arc<AtomicBool>,
    rtc: Option<Rtc>, timeout: Instant, mid: Option<Mid>, pt: Pt, nack: bool,
    manual_nack: bool, nack_cache: VecDeque<CachedPacket>,
    ssrc: Ssrc, rtx_ssrc: Ssrc, profile: String,
    remote_endpoints: [Option<(Protocol, SocketAddr)>; MAX_REMOTE_ENDPOINTS],
    connected: Option<Instant>, last_media: Option<Instant>,
    queue: VecDeque<Arc<Frame>>, bytes: usize, sending: Option<Sending>,
    need_idr: bool, previous: u64, base_ns: Option<u64>, seq: u64,
    pace_next: Instant, closing: bool, congestion: u32,
    budget: Arc<AtomicUsize>, reserved: usize,
}
impl Drop for Session {
    fn drop(&mut self) { self.budget.fetch_sub(self.reserved, Ordering::Relaxed); }
}
impl Session {
    fn lose(&mut self) {
        if self.sending.as_ref().is_some_and(|s| s.remaining != 0) { self.closing = true; }
        self.need_idr = true;
        self.queue.clear();
        self.bytes = 0;
        self.sending = None;
    }
    fn enqueue(&mut self, frame: &Arc<Frame>, limit: usize, now: Instant) {
        if self.connected.is_none() || self.closing { return; }
        if self.queue.len() + usize::from(self.sending.is_some()) >= limit || self.bytes + frame.bytes > SESSION_BYTES || now.duration_since(frame.arrived) > MAX_AGE {
            self.lose();
            self.congestion = self.congestion.saturating_add(1);
            if self.congestion >= 20 { self.closing = true; return; }
        }
        if self.need_idr && !frame.key { return; }
        // Retaining an IDR establishes the dependency chain immediately, not
        // when the pacer eventually finishes sending it.
        if frame.key { self.need_idr = false; }
        self.bytes += frame.bytes;
        self.queue.push_back(frame.clone());
    }
}
struct CachedPacket {
    sequence: u16, created: Instant, protocol: Protocol,
    source: SocketAddr, destination: SocketAddr, bytes: Vec<u8>,
}
struct PendingWrite {
    packet: Vec<u8>, offset: usize, header: [u8; 2], queued: Instant,
    media: Option<(usize, u64)>,
}
struct TcpPeer {
    stream: TcpStream, local: SocketAddr, remote: SocketAddr, owner: Option<usize>,
    input: Vec<u8>, output: VecDeque<PendingWrite>, bytes: usize,
    created: Instant, last_read: Instant,
    writable: bool,
}
struct Reactor {
    config: Arc<Config>, video: UnixDatagram, udp: UdpSocket, listener: TcpListener,
    poll: Poll, commands: Receiver<Create>, shared: Arc<Shared>,
    sources: [Source; 4], sessions: [Option<Session>; 8], tcp: [Option<TcpPeer>; MAX_TCP],
    budget: Arc<AtomicUsize>, next_ssrc: u32,
}
impl Reactor {
    fn run(mut self) {
        let mut events = Events::with_capacity(64);
        let mut video = vec![0u8; 80 + CHUNK];
        let mut network = [0u8; 65536];
        while !self.shared.stop.load(Ordering::Acquire) {
            let now = Instant::now();
            self.commands(now);
            let mut busy = self.read_video(&mut video);
            busy |= self.read_udp(&mut network);
            busy |= self.accept_tcp(now);
            for index in 0..MAX_TCP {
                if self.tcp[index].is_some() { busy |= self.service_tcp(index); }
            }
            self.maintain(Instant::now());
            self.publish(Instant::now());
            let now = Instant::now();
            let mut next = now + Duration::from_millis(20);
            for session in self.sessions.iter().flatten() {
                next = next.min(session.timeout);
                if session.connected.is_some() && (session.sending.is_some() || !session.queue.is_empty()) {
                    // Batch due packets on one 2ms reactor wake instead of a
                    // timer/task notification for each RTP fragment.
                    next = next.min(now + Duration::from_millis(2));
                }
            }
            let wait = if busy { Duration::ZERO } else { next.saturating_duration_since(now) };
            if let Err(error) = self.poll.poll(&mut events, Some(wait)) {
                if error.kind() != io::ErrorKind::Interrupted { break; }
            }
        }
        for index in 0..8 { self.remove(index, stopping()); }
        for index in 0..MAX_TCP { self.close_tcp(index); }
        while let Ok(command) = self.commands.try_recv() { let _ = command.reply.send(Err(stopping())); }
        self.shared.demand.store(0, Ordering::Release);
        self.shared.finished.send_replace(true);
    }

    fn commands(&mut self, now: Instant) {
        let deleted = {
            let mut state = self.shared.state.lock().unwrap();
            std::mem::take(&mut state.deleted)
        };
        for id in deleted {
            if let Some(index) = self.sessions.iter().position(|s| s.as_ref().is_some_and(|s| s.id == id)) {
                self.remove(index, ApiError::new(503, "session", "session closed"));
            }
        }
        for _ in 0..32 {
            let Ok(command) = self.commands.try_recv() else { break; };
            if command.reply.is_closed() { continue; }
            let error = if command.camera >= 4 || self.config.camera_mask & (1 << command.camera) == 0 {
                Some(ApiError::new(404, "camera", "camera is not enabled"))
            } else if self.config.codec != "h264" {
                Some(ApiError::new(422, "codec", "Web video supports H.264 only"))
            } else if !self.config.web_enabled || self.shared.stop.load(Ordering::Acquire) {
                Some(stopping())
            } else if !negotiation::valid_offer(&command.offer) {
                Some(ApiError::new(400, "offer", "one receive-only video offer required"))
            } else if self.sessions.iter().flatten().count() >= self.config.max_video_sessions.min(8)
                || self.sessions.iter().flatten().filter(|s| s.camera == command.camera).count() >= 2 {
                Some(ApiError::new(429, "limit", "video reader limit reached"))
            } else { None };
            if let Some(error) = error { let _ = command.reply.send(Err(error)); continue; }
            let Some(index) = self.sessions.iter().position(Option::is_none) else {
                let _ = command.reply.send(Err(ApiError::new(429, "limit", "video reader limit reached")));
                continue;
            };
            if !reserve(&self.budget, TRANSPORT_RESERVE) {
                let _ = command.reply.send(Err(ApiError::new(503, "media", "media payload budget exhausted")));
                continue;
            }
            let id = loop {
                let candidate = format!("{:032x}", rand::random::<u128>());
                if !self.sessions.iter().flatten().any(|s| s.id == candidate) { break candidate; }
            };
            let ssrc = self.allocate_ssrc();
            let rtx_ssrc = self.allocate_ssrc();
            self.sessions[index] = Some(Session {
                cancelled: command.cancelled,
                id, camera: command.camera, created: now, reply: Some(command.reply), offer: Some(command.offer),
                rtc: None, timeout: now + HANDSHAKE, mid: None, pt: 96.into(), nack: false,
                manual_nack: false, nack_cache: VecDeque::new(),
                ssrc, rtx_ssrc, profile: String::new(), connected: None, last_media: None,
                remote_endpoints: [None; MAX_REMOTE_ENDPOINTS],
                queue: VecDeque::with_capacity(self.queue_limit()), bytes: 0, sending: None,
                need_idr: true, previous: 0, base_ns: None, seq: rand::random::<u16>() as u64,
                pace_next: now, closing: false, congestion: 0, budget: self.budget.clone(), reserved: TRANSPORT_RESERVE,
            });
            self.shared.state.lock().unwrap().ids[index] = self.sessions[index].as_ref().map(|s| s.id.clone());
        }
        self.shared.demand.store(self.sessions.iter().flatten().fold(0, |bits, s| bits | (1 << s.camera)), Ordering::Release);
    }

    fn queue_limit(&self) -> usize { 4.max(self.config.fps.div_ceil(4) as usize) }

    fn allocate_ssrc(&mut self) -> Ssrc {
        loop {
            self.next_ssrc = self.next_ssrc.wrapping_add(1);
            let ssrc: Ssrc = self.next_ssrc.into();
            if self.next_ssrc != 0 && !self.sessions.iter().flatten().any(|s| s.ssrc == ssrc || s.rtx_ssrc == ssrc) { return ssrc; }
        }
    }

    fn remove(&mut self, index: usize, error: ApiError) {
        let Some(mut session) = self.sessions[index].take() else { return; };
        if let Some(reply) = session.reply.take() { let _ = reply.send(Err(error)); }
        // Dropping the sans-I/O owner releases DTLS, SRTP, cache and queues
        // synchronously; quota is not reusable before this destruction.
        drop(session);
        for peer in 0..MAX_TCP {
            if self.tcp[peer].as_ref().is_some_and(|p| p.owner == Some(index)) { self.close_tcp(peer); }
        }
        self.shared.state.lock().unwrap().ids[index] = None;
    }

    fn maintain(&mut self, now: Instant) {
        for camera in 0..4 {
            if self.sources[camera].partial.as_ref().is_some_and(|a| now.duration_since(a.arrived) > MAX_AGE) {
                self.sources[camera].lost();
                self.source_gap(camera);
            }
        }
        for index in 0..8 {
            let Some(mut session) = self.sessions[index].take() else { continue; };
            if session.cancelled.load(Ordering::Acquire) || session.reply.as_ref().is_some_and(oneshot::Sender::is_closed) { session.closing = true; }
            while session.nack_cache.front().is_some_and(|p| now.duration_since(p.created) > Duration::from_secs(3)) { session.nack_cache.pop_front(); }
            if session.connected.is_none() && now.duration_since(session.created) >= HANDSHAKE { session.closing = true; }
            if session.connected.is_some() && now.duration_since(session.last_media.or(session.connected).unwrap()) >= Duration::from_secs(3) { session.closing = true; }
            if session.rtc.is_none() && !session.closing {
                let source = &self.sources[session.camera];
                if !source.profile.is_empty() && !source.pps.is_empty() && source.last.is_some_and(|last| now.duration_since(last) <= SOURCE_FRESH) {
                    let profile = source.profile.clone();
                    if let Err(error) = self.negotiate(index, &mut session, &profile, now) {
                        if let Some(reply) = session.reply.take() { let _ = reply.send(Err(error)); }
                        session.closing = true;
                    }
                }
            }
            if !session.closing && session.rtc.is_some() && session.timeout <= now {
                if session.rtc.as_mut().unwrap().handle_input(Input::Timeout(now)).is_err() { session.closing = true; }
                self.drain(index, &mut session, now);
            }
            if !session.closing && session.connected.is_some() { self.pace(index, &mut session, now); }
            let closing = session.closing || session.rtc.as_ref().is_some_and(|rtc| !rtc.is_alive());
            self.sessions[index] = Some(session);
            if closing {
                let error = if self.sessions[index].as_ref().unwrap().rtc.is_none() {
                    ApiError::new(503, "source", "source parameter sets unavailable")
                } else { ApiError::new(503, "session", "session closed") };
                self.remove(index, error);
            }
        }
    }

    fn publish(&self, now: Instant) {
        let mut snapshot = self.shared.state.lock().unwrap();
        let mut demand = 0;
        for camera in 0..4 {
            let source = &self.sources[camera];
            let active = self.sessions.iter().flatten().any(|s| s.camera == camera);
            if active { demand |= 1 << camera; }
            snapshot.cameras[camera] = CameraStatus {
                camera_id: camera, ipc_drops: source.drops,
                web: if initial_state(&self.config, camera) != "idle" { initial_state(&self.config, camera) }
                    else if !active { "idle" }
                    else if !source.need_idr && source.last.is_some_and(|last| now.duration_since(last) <= SOURCE_FRESH) { "flowing" }
                    else { "waiting-keyframe" },
            };
        }
        for index in 0..8 {
            if snapshot.ids[index].as_deref() != self.sessions[index].as_ref().map(|s| s.id.as_str()) {
                snapshot.ids[index] = self.sessions[index].as_ref().map(|s| s.id.clone());
            }
        }
        self.shared.demand.store(demand, Ordering::Release);
    }
}

impl Reactor {
    fn read_video(&mut self, buffer: &mut [u8]) -> bool {
        for _ in 0..128 {
            let n = unsafe { libc::recv(self.video.as_raw_fd(), buffer.as_mut_ptr().cast(), buffer.len(), libc::MSG_DONTWAIT | libc::MSG_TRUNC) };
            if n < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted { continue; }
                if error.kind() != io::ErrorKind::WouldBlock { self.shared.stop.store(true, Ordering::Release); }
                return false;
            }
            if n < 80 { continue; }
            if n as usize > buffer.len() {
                let camera = be32(buffer, 24) as usize;
                if camera < 4 && be32(buffer, 0) == 0x53575631 && be64(buffer, 8) == self.config.generation {
                    self.sources[camera].lost(); self.source_gap(camera);
                }
                continue;
            }
            let data = &buffer[..n as usize];
            if be32(data, 0) != 0x53575631 || be16(data, 4) != 1 || be16(data, 6) != 80 || be64(data, 8) != self.config.generation { continue; }
            let camera = be32(data, 24) as usize;
            if camera >= 4 || self.config.camera_mask & (1 << camera) == 0 || !self.sessions.iter().flatten().any(|s| s.camera == camera) { continue; }
            self.ingest(camera, data, Instant::now());
        }
        true
    }

    fn ingest(&mut self, camera: usize, data: &[u8], now: Instant) {
        let source = &mut self.sources[camera];
        let epoch = be64(data, 16);
        if epoch == 0 || epoch < source.epoch { return; }
        let mut revoke = false;
        if epoch != source.epoch {
            revoke = source.epoch != 0;
            let drops = source.drops;
            *source = Source::default();
            source.drops = drops;
            source.epoch = epoch;
        }
        if revoke { self.revoke_camera(camera); }
        let source = &mut self.sources[camera];
        let total = be32(data, 36) as usize;
        let sequence = be64(data, 40);
        let source_ns = be64(data, 48);
        let media_ns = be64(data, 56);
        let offset = be32(data, 64) as usize;
        let index = be16(data, 68);
        let count = be16(data, 70);
        let size = be32(data, 72) as usize;
        let flags = be32(data, 32);
        if be32(data, 28) != 1 || total == 0 || total > MAX_AU || size == 0 || size > CHUNK || size != data.len() - 80
            || offset != index as usize * CHUNK || count as usize != total.div_ceil(CHUNK) || index >= count
            || offset >= total || size != CHUNK.min(total - offset) || be32(data, 76) != 0 {
            source.lost();
            self.source_gap(camera);
            return;
        }
        let mut gap = false;
        if source.partial.as_ref().is_some_and(|a| now.duration_since(a.arrived) > MAX_AGE) { source.lost(); gap = true; }
        if index == 0 {
            if source.partial.is_some() { source.lost(); gap = true; }
            if source.sequence != 0 && source.sequence.checked_add(1) != Some(sequence) {
                source.need_idr = true;
                source.drops = source.drops.saturating_add(1);
                gap = true;
            }
            if sequence <= source.sequence || (source.sequence != 0 && media_ns < source.media_ns) || !reserve(&self.budget, total) {
                source.lost(); self.source_gap(camera); return;
            }
            source.partial = Some(Assembly {
                payload: Payload { bytes: vec![0; total], budget: self.budget.clone(), reserved: total },
                sequence, media_ns, source_ns, flags, epoch, next: 0, count, arrived: now,
            });
        }
        let valid = source.partial.as_ref().is_some_and(|a| a.sequence == sequence && a.epoch == epoch && a.media_ns == media_ns
            && a.source_ns == source_ns && a.flags == flags && a.next == index && a.count == count && a.payload.bytes.len() == total);
        if !valid { source.lost(); self.source_gap(camera); return; }
        let assembly = source.partial.as_mut().unwrap();
        assembly.payload.bytes[offset..offset + size].copy_from_slice(&data[80..]);
        assembly.next += 1;
        if assembly.next != count {
            if gap { self.source_gap(camera); }
            return;
        }
        let assembly = source.partial.take().unwrap();
        source.sequence = sequence;
        source.media_ns = media_ns;
        let mut sps = None;
        let mut pps = None;
        let mut key = false;
        let mut vcl = false;
        let mut malformed = false;
        for nal in Nals::new(&assembly.payload.bytes) {
            if nal.is_empty() || nal[0] & 0x80 != 0 { malformed = true; break; }
            match nal[0] & 31 {
                1..=4 => vcl = true,
                5 => { key = true; vcl = true; }
                7 => { if !(4..=4096).contains(&nal.len()) { malformed = true; break; } sps = Some(nal); }
                8 => { if nal.len() > 4096 { malformed = true; break; } pps = Some(nal); }
                0 | 24..=31 => { malformed = true; break; }
                _ => {}
            }
        }
        if malformed || (!vcl && sps.is_none() && pps.is_none()) {
            source.lost(); self.source_gap(camera); return;
        }
        let changed = sps.is_some_and(|v| !source.sps.is_empty() && v != source.sps) || pps.is_some_and(|v| !source.pps.is_empty() && v != source.pps);
        if let Some(sps) = sps {
            source.sps.clear(); source.sps.extend_from_slice(sps);
            source.profile = format!("{:02x}{:02x}{:02x}", sps[1], sps[2], sps[3]);
        }
        if let Some(pps) = pps { source.pps.clear(); source.pps.extend_from_slice(pps); }
        if changed { source.need_idr = true; }
        if gap || changed { self.source_gap(camera); }
        if changed { self.revoke_camera(camera); }
        let source = &mut self.sources[camera];
        if !source.sps.is_empty() && !source.pps.is_empty() { source.last = Some(now); }
        if !vcl || source.need_idr && (!key || source.sps.is_empty() || source.pps.is_empty()) { return; }
        source.need_idr = false;
        if !self.sessions.iter().flatten().any(|session| session.camera == camera && session.connected.is_some() && !session.closing) { return; }
        let prefix_sps = key && sps.is_none();
        let prefix_pps = key && pps.is_none();
        let bytes = total + if prefix_sps { source.sps.len() + 4 } else { 0 } + if prefix_pps { source.pps.len() + 4 } else { 0 };
        // Packet payload Arcs are shared between every reader. str0m owns Arc
        // payloads, so split once here rather than copying a full AU per peer.
        let mut packet_count = Nals::new(&assembly.payload.bytes).map(|nal| nal.len().saturating_sub(1).div_ceil(RTP_PAYLOAD - 2).max(1)).sum::<usize>();
        if prefix_sps { packet_count += source.sps.len().saturating_sub(1).div_ceil(RTP_PAYLOAD - 2).max(1); }
        if prefix_pps { packet_count += source.pps.len().saturating_sub(1).div_ceil(RTP_PAYLOAD - 2).max(1); }
        let reservation = bytes.saturating_add(packet_count.saturating_mul(2 + 2 * std::mem::size_of::<Arc<[u8]>>() + 2 * std::mem::size_of::<usize>()));
        if bytes > MAX_AU || !reserve(&self.budget, reservation) { source.lost(); self.source_gap(camera); return; }
        let mut packets = Vec::with_capacity(packet_count);
        if prefix_sps { packetize_nal(&source.sps, &mut packets); }
        if prefix_pps { packetize_nal(&source.pps, &mut packets); }
        for nal in Nals::new(&assembly.payload.bytes) { packetize_nal(nal, &mut packets); }
        source.need_idr = false;
        let frame = Arc::new(Frame { packets: packets.into_boxed_slice(), budget: self.budget.clone(), reserved: reservation,
            bytes, sequence, media_ns, arrived: assembly.arrived, key });
        let limit = self.queue_limit();
        for session in self.sessions.iter_mut().flatten().filter(|s| s.camera == camera) { session.enqueue(&frame, limit, now); }
    }

    fn source_gap(&mut self, camera: usize) {
        for index in 0..8 {
            if let Some(session) = self.sessions[index].as_mut().filter(|s| s.camera == camera) {
                if self.tcp.iter().flatten().any(|p| p.output.iter().any(|w| w.media.is_some_and(|(owner, _)| owner == index))) { session.closing = true; }
                session.lose();
            }
        }
    }

    fn revoke_camera(&mut self, camera: usize) {
        for index in 0..8 {
            if self.sessions[index].as_ref().is_some_and(|s| s.camera == camera) {
                self.remove(index, ApiError::new(503, "session", "source configuration changed"));
            }
        }
    }
}

fn be16(b: &[u8], offset: usize) -> u16 { u16::from_be_bytes(b[offset..offset + 2].try_into().unwrap()) }
fn be32(b: &[u8], offset: usize) -> u32 { u32::from_be_bytes(b[offset..offset + 4].try_into().unwrap()) }
fn be64(b: &[u8], offset: usize) -> u64 { u64::from_be_bytes(b[offset..offset + 8].try_into().unwrap()) }

struct Nals<'a> { data: &'a [u8], position: usize }
impl<'a> Nals<'a> { fn new(data: &'a [u8]) -> Self { Self { data, position: 0 } } }
fn start_code(data: &[u8], from: usize) -> Option<(usize, usize)> {
    let mut i = from;
    while i + 3 <= data.len() {
        if data[i] == 0 && data[i + 1] == 0 {
            if data[i + 2] == 1 { return Some((i, 3)); }
            if i + 4 <= data.len() && data[i + 2] == 0 && data[i + 3] == 1 { return Some((i, 4)); }
        }
        i += 1;
    }
    None
}
impl<'a> Iterator for Nals<'a> {
    type Item = &'a [u8];
    fn next(&mut self) -> Option<Self::Item> {
        let (begin, prefix) = start_code(self.data, self.position)?;
        let start = begin + prefix;
        let end = start_code(self.data, start).map_or(self.data.len(), |(at, _)| at);
        self.position = end;
        let mut end = end;
        while end > start && self.data[end - 1] == 0 { end -= 1; }
        Some(&self.data[start..end])
    }
}
fn packetize_nal(nal: &[u8], packets: &mut Vec<Arc<[u8]>>) {
    if nal.is_empty() || matches!(nal[0] & 31, 9 | 12) { return; }
    if nal.len() <= RTP_PAYLOAD { packets.push(Arc::from(nal)); return; }
    let indicator = (nal[0] & 0xe0) | 28;
    let kind = nal[0] & 31;
    let mut position = 1;
    while position < nal.len() {
        let count = (nal.len() - position).min(RTP_PAYLOAD - 2);
        let mut packet = Arc::<[u8]>::new_uninit_slice(count + 2);
        let target = Arc::get_mut(&mut packet).unwrap();
        target[0].write(indicator);
        target[1].write(kind | if position == 1 { 0x80 } else { 0 } | if position + count == nal.len() { 0x40 } else { 0 });
        for (slot, byte) in target[2..].iter_mut().zip(&nal[position..position + count]) { slot.write(*byte); }
        packets.push(unsafe { packet.assume_init() });
        position += count;
    }
}

impl Reactor {
    fn negotiate(&mut self, index: usize, session: &mut Session, profile: &str, now: Instant) -> Result<(), ApiError> {
        let selection = negotiation::select(session.offer.as_deref().unwrap(), profile)?;
        let addresses = interface_addresses(&self.config.listen_address).map_err(|_| ApiError::new(503, "network", "local addresses unavailable"))?;
        if addresses.is_empty() { return Err(ApiError::new(503, "network", "local addresses unavailable")); }
        let mut builder = RtcConfig::new().clear_codecs().set_ice_lite(true).set_rtp_mode(true)
            .set_send_buffer_video(512).set_stats_interval(None)
            .enable_raw_packets(selection.nack && selection.resend.is_none());
        let mut params = PayloadParams::new(selection.payload.into(), selection.resend.map(Into::into), CodecSpec {
            codec: Codec::H264, clock_rate: Frequency::NINETY_KHZ, channels: None,
            format: FormatParams { packetization_mode: Some(1), profile_level_id: Some(selection.profile), level_asymmetry_allowed: Some(false), ..Default::default() },
        });
        params.set_fb_nack(selection.nack);
        params.set_fb_pli(selection.pli);
        params.set_fb_fir(selection.fir);
        params.set_fb_remb(false);
        params.set_fb_transport_cc(false);
        *builder.codec_config() = str0m::format::CodecConfig::new_from_payload_params(vec![params]);
        session.rtc = Some(builder.build(now));
        self.drain(index, session, now);
        for ip in addresses {
            let address = SocketAddr::new(IpAddr::V4(ip), self.config.ice_port);
            let candidate = Candidate::host(address, Protocol::Udp).map_err(|_| ApiError::new(503, "network", "local candidate unavailable"))?;
            session.rtc.as_mut().unwrap().add_local_candidate(candidate);
            self.drain(index, session, now);
            let candidate = Candidate::builder().tcp().host(address).tcptype(TcpType::Passive).build()
                .map_err(|_| ApiError::new(503, "network", "local candidate unavailable"))?;
            session.rtc.as_mut().unwrap().add_local_candidate(candidate);
            self.drain(index, session, now);
        }
        let answer = session.rtc.as_mut().unwrap().sdp_api().accept_offer(selection.offer);
        self.drain(index, session, now);
        let answer = answer.map_err(|_| {
            let mut error = ApiError::new(422, "negotiation", "H.264 source profile cannot be negotiated");
            error.source_profile = Some(profile.to_owned());
            error
        })?;
        let mut sdp = answer.to_sdp_string();
        // MediaAdded is deferred until DTLS; the accepted SDP already binds MID.
        let mid = sdp.lines().find_map(|line| line.strip_prefix("a=mid:")).map(|value| Mid::from(value.trim()));
        let Some(mid) = mid.filter(|mid| session.rtc.as_ref().unwrap().media(*mid).is_some_and(|media| !media.disabled())) else {
            return Err(ApiError::new(422, "negotiation", "source codec cannot be selected"));
        };
        session.mid = Some(mid);
        let old = outgoing_ssrcs(&sdp).ok_or_else(|| ApiError::new(503, "media", "sender initialization failed"))?;
        // Remove the SDP-generated stream before declaring the reserved SSRC;
        // reset_stream_tx changes its contents but retains the old lookup key.
        session.rtc.as_mut().unwrap().direct_api().remove_stream_tx(old.0.into());
        self.drain(index, session, now);
        session.rtc.as_mut().unwrap().direct_api().declare_stream_tx(session.ssrc, selection.resend.map(|_| session.rtx_ssrc), mid, None);
        self.drain(index, session, now);
        {
            let mut direct = session.rtc.as_mut().unwrap().direct_api();
            let stream = direct.stream_tx(&session.ssrc).ok_or_else(|| ApiError::new(503, "media", "sender initialization failed"))?;
            stream.set_rtx_cache(512, Duration::from_secs(3), Some(0.15));
        }
        self.drain(index, session, now);
        // SDP API generates a sender SSRC before the hub-wide reservation can
        // be applied. Rewrite only SSRC attributes to match the reset stream.
        sdp = rewrite_ssrcs(&sdp, old, (*session.ssrc, selection.resend.map(|_| *session.rtx_ssrc)));
        session.profile = profile.to_owned();
        session.pt = selection.payload.into();
        session.nack = selection.nack;
        session.manual_nack = selection.nack && selection.resend.is_none();
        session.offer = None;
        if session.closing { return Err(ApiError::new(503, "session", "session closed")); }
        if let Some(reply) = session.reply.take() {
            if reply.send(Ok(SessionAnswer { session_id: session.id.clone(), kind: "answer".to_owned(), sdp })).is_err() { session.closing = true; }
        }
        Ok(())
    }

    /// Every RTC mutation calls this before any subsequent mutation. Outputs
    /// are consumed, never parked inside str0m while waiting for socket I/O.
    fn drain(&mut self, index: usize, session: &mut Session, now: Instant) {
        let Some(rtc) = session.rtc.as_mut() else { return; };
        let mut outputs = 0usize;
        loop {
            let output = rtc.poll_output();
            outputs += 1;
            match output {
                Ok(Output::Timeout(at)) => { session.timeout = at; break; }
                Ok(Output::Event(event)) => match event {
                    Event::Connected => { session.connected.get_or_insert(now); session.last_media.get_or_insert(now); }
                    Event::MediaAdded(media) => { session.mid = Some(media.mid); }
                    Event::KeyframeRequest(_) => {
                        session.need_idr = true;
                        session.queue.clear();
                        session.bytes = session.sending.as_ref().map_or(0, |s| s.frame.bytes);
                        if let Some(sending) = session.sending.as_mut() { sending.failed = true; }
                    }
                    Event::IceConnectionStateChange(IceConnectionState::Disconnected) | Event::Closed => { session.closing = true; }
                    Event::RawPacket(raw) => {
                        if let RawPacket::RtcpRx(Rtcp::Nack(nack)) = *raw {
                            if session.manual_nack && nack.ssrc == session.ssrc {
                                let mut replayed = [0u16; 64];
                                let mut count = 0;
                                for report in &nack.reports {
                                    for bit in 0..17u16 {
                                        if bit != 0 && report.blp & (1 << (bit - 1)) == 0 { continue; }
                                        let sequence = report.pid.wrapping_add(bit);
                                        if count == replayed.len() { break; }
                                        if replayed[..count].contains(&sequence) { continue; }
                                        let Some(packet) = session.nack_cache.iter().rev().find(|p| p.sequence == sequence && now.duration_since(p.created) <= Duration::from_secs(3)) else { continue; };
                                        replayed[count] = sequence;
                                        count += 1;
                                        let transmit = Transmit { proto: packet.protocol, source: packet.source, destination: packet.destination, contents: packet.bytes.clone().into() };
                                        if matches!(self.transmit(transmit, None, now), SendResult::Closed) { session.closing = true; }
                                    }
                                }
                            }
                        }
                    }
                    _ => {}
                },
                Ok(Output::Transmit(transmit)) => {
                    let primary = is_primary(&transmit.contents, *session.ssrc);
                    let media = if primary { session.sending.as_ref().map(|s| (index, s.frame.sequence)) } else { None };
                    if session.manual_nack && primary && transmit.contents.len() <= 1400 {
                        while session.nack_cache.len() >= 512 || session.nack_cache.front().is_some_and(|p| now.duration_since(p.created) > Duration::from_secs(3)) { session.nack_cache.pop_front(); }
                        session.nack_cache.push_back(CachedPacket { sequence: be16(&transmit.contents, 2), created: now, protocol: transmit.proto, source: transmit.source, destination: transmit.destination, bytes: transmit.contents.to_vec() });
                    }
                    let result = self.transmit(transmit, media, now);
                    match result {
                        SendResult::Sent => {
                            if primary { if let Some(sending) = session.sending.as_mut() { sending.remaining = sending.remaining.saturating_sub(1); } }
                        }
                        SendResult::Queued => {}
                        SendResult::Lost => {
                            if primary {
                                session.need_idr = true;
                                session.queue.clear();
                                session.bytes = session.sending.as_ref().map_or(0, |s| s.frame.bytes);
                                if let Some(sending) = session.sending.as_mut() { sending.failed = true; sending.remaining = sending.remaining.saturating_sub(1); }
                            }
                        }
                        SendResult::Closed => session.closing = true,
                    }
                }
                Err(_) => { session.closing = true; break; }
            }
            // A bounded input can elicit a large NACK burst. Discarding the
            // owner after draining keeps neither a growing output list nor a
            // busy peer indefinitely in the shared scheduling round.
            if outputs > 4096 { session.closing = true; }
        }
    }

    fn pace(&mut self, index: usize, session: &mut Session, now: Instant) {
        let rate = (self.config.bitrate_bps / 4).max(125_000);
        let burst = Duration::from_nanos((4 * 1222 * 1_000_000_000u64) / rate);
        for _ in 0..64 {
            if session.closing { break; }
            if let Some(sending) = session.sending.as_ref() {
                if now.duration_since(sending.frame.arrived) >= MAX_AGE || sending.failed {
                    // A partially written RFC4571 frame cannot be truncated;
                    // closing this transport is the only safe recovery.
                    let pending_tcp = self.tcp.iter().flatten().any(|p| p.output.iter().any(|w| w.media == Some((index, sending.frame.sequence))));
                    if pending_tcp { session.closing = true; break; }
                    session.lose();
                    continue;
                }
                if sending.packet == sending.frame.packets.len() {
                    if sending.remaining != 0 { break; }
                    let sending = session.sending.take().unwrap();
                    session.bytes = session.bytes.saturating_sub(sending.frame.bytes);
                    session.last_media = Some(now);
                    session.congestion = 0;
                    continue;
                }
            }
            if session.sending.is_none() {
                let Some(frame) = session.queue.pop_front() else { break; };
                if now.duration_since(frame.arrived) >= MAX_AGE || session.previous != 0 && session.previous.checked_add(1) != Some(frame.sequence) && !frame.key {
                    session.lose();
                    continue;
                }
                if session.need_idr && !frame.key { session.bytes = session.bytes.saturating_sub(frame.bytes); continue; }
                if frame.key { session.need_idr = false; }
                session.previous = frame.sequence;
                let base = *session.base_ns.get_or_insert(frame.media_ns);
                let Some(elapsed) = frame.media_ns.checked_sub(base) else { session.lose(); continue; };
                let timestamp = ((elapsed / 1_000_000_000) * 90000 + (elapsed % 1_000_000_000) * 90000 / 1_000_000_000) as u32;
                session.sending = Some(Sending { frame, packet: 0, timestamp, failed: false, remaining: 0 });
            }
            let sending = session.sending.as_mut().unwrap();
            if sending.remaining >= 64 { break; }
            let payload = &sending.frame.packets[sending.packet];
            let packet_time = Duration::from_nanos(((payload.len() + 22) as u64 * 1_000_000_000).div_ceil(rate));
            session.pace_next = session.pace_next.max(now.checked_sub(burst).unwrap_or(now));
            let ready = session.pace_next + packet_time;
            if ready > now { break; }
            if ready.saturating_duration_since(sending.frame.arrived) > MAX_AGE { session.lose(); continue; }
            session.pace_next = ready;
            let marker = sending.packet + 1 == sending.frame.packets.len();
            let packet = RtpWrite::new(session.pt, session.seq.into(), sending.timestamp, now, payload.clone()).marker(marker).nackable(session.nack);
            sending.packet += 1;
            sending.remaining += 1;
            session.seq += 1;
            let Some(rtc) = session.rtc.as_mut() else { session.closing = true; break; };
            let mut direct = rtc.direct_api();
            let Some(stream) = direct.stream_tx(&session.ssrc) else { session.closing = true; break; };
            stream.write_rtp(packet);
            self.drain(index, session, now);
            // write_rtp does not advance str0m time. One timeout after each
            // drained write timestamps and empties that one-packet send queue.
            if session.rtc.as_mut().unwrap().handle_input(Input::Timeout(now)).is_err() { session.closing = true; }
            self.drain(index, session, now);
        }
    }
}

fn outgoing_ssrcs(sdp: &str) -> Option<(u32, Option<u32>)> {
    for line in sdp.lines() {
        if let Some(value) = line.strip_prefix("a=ssrc-group:FID ") {
            let mut parts = value.split_ascii_whitespace();
            return Some((parts.next()?.parse().ok()?, Some(parts.next()?.parse().ok()?)));
        }
    }
    sdp.lines().find_map(|line| line.strip_prefix("a=ssrc:")?.split_ascii_whitespace().next()?.parse().ok().map(|v| (v, None)))
}
fn rewrite_ssrcs(sdp: &str, old: (u32, Option<u32>), new: (u32, Option<u32>)) -> String {
    let mut result = String::with_capacity(sdp.len());
    for line in sdp.lines().map(|l| l.trim_end_matches('\r')) {
        if let Some(value) = line.strip_prefix("a=ssrc:") {
            if let Some((id, rest)) = value.split_once(' ') {
                if id.parse::<u32>() == Ok(old.0) { result.push_str(&format!("a=ssrc:{} {rest}\r\n", new.0)); continue; }
                if old.1.is_some_and(|v| id.parse::<u32>() == Ok(v)) {
                    if let Some(rtx) = new.1 { result.push_str(&format!("a=ssrc:{rtx} {rest}\r\n")); }
                    continue;
                }
            }
        }
        if line.starts_with("a=ssrc-group:FID ") {
            if let Some(rtx) = new.1 { result.push_str(&format!("a=ssrc-group:FID {} {rtx}\r\n", new.0)); }
            continue;
        }
        result.push_str(line); result.push_str("\r\n");
    }
    result
}

fn interface_addresses(listen: &str) -> io::Result<Vec<Ipv4Addr>> {
    let filter: Ipv4Addr = listen.parse().map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid listen address"))?;
    let mut list = std::ptr::null_mut();
    if unsafe { libc::getifaddrs(&mut list) } != 0 { return Err(io::Error::last_os_error()); }
    let mut result = Vec::with_capacity(8);
    let mut current = list;
    while !current.is_null() {
        let interface = unsafe { &*current };
        if !interface.ifa_addr.is_null() && unsafe { (*interface.ifa_addr).sa_family as i32 } == libc::AF_INET && interface.ifa_flags & libc::IFF_UP as u32 != 0 {
            let address = unsafe { &*(interface.ifa_addr as *const libc::sockaddr_in) };
            let ip = Ipv4Addr::from(address.sin_addr.s_addr.to_ne_bytes());
            if (filter.is_unspecified() || ip == filter) && (!ip.is_loopback() || filter.is_loopback()) && !ip.is_unspecified() && !ip.is_multicast() && !result.contains(&ip) { result.push(ip); }
        }
        current = interface.ifa_next;
    }
    unsafe { libc::freeifaddrs(list); }
    Ok(result)
}

enum SendResult { Sent, Queued, Lost, Closed }
fn is_primary(data: &[u8], ssrc: u32) -> bool {
    data.len() >= 12 && data[0] & 0xc0 == 0x80 && !(192..=223).contains(&data[1]) && be32(data, 8) == ssrc
}

impl Reactor {
    fn read_udp(&mut self, buffer: &mut [u8]) -> bool {
        for _ in 0..128 {
            match receive_udp(&self.udp, buffer) {
                Ok((length, source, destination)) => { self.route(Protocol::Udp, source, destination, &buffer[..length], None); }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return false,
                Err(_) => return false,
            }
        }
        true
    }

    fn route(&mut self, protocol: Protocol, source: SocketAddr, destination: SocketAddr, data: &[u8], tcp: Option<usize>) {
        let IpAddr::V4(local_ip) = destination.ip() else { return; };
        if self.config.listen_address != "0.0.0.0" && self.config.listen_address.parse::<Ipv4Addr>().ok() != Some(local_ip)
            || local_ip.is_loopback() && self.config.listen_address != "127.0.0.1" { return; }
        let Ok(receive) = Receive::new(protocol, source, destination, data) else { return; };
        let now = Instant::now();
        let input = Input::Receive(now, receive);
        let owner = tcp.and_then(|peer| self.tcp[peer].as_ref().and_then(|p| p.owner));
        let selected = if let Some(index) = owner {
            self.sessions[index].as_ref().filter(|s| s.rtc.as_ref().is_some_and(|rtc| rtc.accepts(&input))).map(|_| index)
        } else {
            self.sessions.iter().position(|s| s.as_ref().is_some_and(|s| s.rtc.as_ref().is_some_and(|rtc| rtc.accepts(&input))))
        };
        let Some(index) = selected else { return; };
        // ICE retains discovered endpoint history until RTC destruction. Bound
        // lifetime admission, not just concurrently open candidate sockets.
        let session = self.sessions[index].as_mut().unwrap();
        let endpoint = (protocol, source);
        if !session.remote_endpoints.contains(&Some(endpoint)) {
            let Some(slot) = session.remote_endpoints.iter_mut().find(|slot| slot.is_none()) else {
                if let Some(peer) = tcp { self.close_tcp(peer); }
                return;
            };
            *slot = Some(endpoint);
        }
        if let Some(peer) = tcp { if let Some(peer) = self.tcp[peer].as_mut() { peer.owner = Some(index); } }
        let mut session = self.sessions[index].take().unwrap();
        if session.rtc.as_mut().unwrap().handle_input(input).is_err() { session.closing = true; }
        self.drain(index, &mut session, now);
        self.sessions[index] = Some(session);
    }

    fn transmit(&mut self, transmit: Transmit, media: Option<(usize, u64)>, now: Instant) -> SendResult {
        match transmit.proto {
            Protocol::Udp => match send_udp(&self.udp, &transmit) {
                Ok(n) if n == transmit.contents.len() => SendResult::Sent,
                Ok(_) => SendResult::Lost,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock || error.kind() == io::ErrorKind::Interrupted || matches!(error.raw_os_error(), Some(libc::ENOBUFS) | Some(libc::ENETUNREACH) | Some(libc::EHOSTUNREACH)) => SendResult::Lost,
                Err(_) => SendResult::Closed,
            },
            Protocol::Tcp => {
                let Some(index) = self.tcp.iter().position(|peer| peer.as_ref().is_some_and(|p| p.local == transmit.source && p.remote == transmit.destination)) else { return if media.is_some() { SendResult::Closed } else { SendResult::Lost }; };
                let peer = self.tcp[index].as_mut().unwrap();
                let length = transmit.contents.len();
                if length > u16::MAX as usize || peer.bytes + length + 2 > TCP_BYTES || peer.output.len() >= 512 { return SendResult::Closed; }
                peer.bytes += length + 2;
                peer.output.push_back(PendingWrite { header: (length as u16).to_be_bytes(), packet: transmit.contents.into(), offset: 0, queued: now, media });
                let fd = peer.stream.as_raw_fd();
                if !peer.writable {
                    if self.poll.registry().reregister(&mut SourceFd(&fd), Token(TCP_TOKEN + index), Interest::READABLE | Interest::WRITABLE).is_err() { return SendResult::Closed; }
                    peer.writable = true;
                }
                SendResult::Queued
            }
            _ => SendResult::Closed,
        }
    }

    fn accept_tcp(&mut self, now: Instant) -> bool {
        for _ in 0..16 {
            let (stream, remote) = match self.listener.accept() {
                Ok(value) => value,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return false,
            };
            let Some(index) = self.tcp.iter().position(Option::is_none) else { continue; };
            if !remote.is_ipv4() || stream.set_nonblocking(true).is_err() || stream.set_nodelay(true).is_err() { continue; }
            let Ok(local) = stream.local_addr() else { continue; };
            if !reserve(&self.budget, 2 * TCP_BYTES) { continue; }
            let fd = stream.as_raw_fd();
            if self.poll.registry().register(&mut SourceFd(&fd), Token(TCP_TOKEN + index), Interest::READABLE).is_err() {
                self.budget.fetch_sub(2 * TCP_BYTES, Ordering::Relaxed); continue;
            }
            self.tcp[index] = Some(TcpPeer { stream, local, remote, owner: None,
                input: Vec::with_capacity(TCP_BYTES + 1), output: VecDeque::new(), bytes: 0, created: now, last_read: now, writable: false });
        }
        true
    }

    fn close_tcp(&mut self, index: usize) {
        let Some(peer) = self.tcp[index].take() else { return; };
        let _ = self.poll.registry().deregister(&mut SourceFd(&peer.stream.as_raw_fd()));
        // Authentication associates candidate probes with an RTC, not with its
        // selected transport. Closing an unused probe must not revoke the RTC.
        // A lost media route is handled by transmit failure or RTC liveness.
        self.budget.fetch_sub(2 * TCP_BYTES, Ordering::Relaxed);
    }

    fn service_tcp(&mut self, index: usize) -> bool {
        let now = Instant::now();
        let peer = self.tcp[index].as_ref().unwrap();
        if peer.owner.is_none() && now.duration_since(peer.created) >= HANDSHAKE
            || peer.output.front().is_some_and(|w| now.duration_since(w.queued) >= Duration::from_secs(1))
            || now.duration_since(peer.last_read) >= HANDSHAKE {
            self.close_tcp(index); return false;
        }
        if !self.flush_tcp(index) { self.close_tcp(index); return false; }
        let mut peer = self.tcp[index].take().unwrap();
        let mut busy = false;
        let mut scratch = [0u8; 8192];
        for _ in 0..16 {
            let available = (TCP_BYTES + 1).saturating_sub(peer.input.len()).min(scratch.len());
            if available == 0 { break; }
            match peer.stream.read(&mut scratch[..available]) {
                Ok(0) => { self.tcp[index] = Some(peer); self.close_tcp(index); return false; }
                Ok(n) => { peer.input.extend_from_slice(&scratch[..n]); peer.last_read = now; busy = true; }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(_) => { self.tcp[index] = Some(peer); self.close_tcp(index); return false; }
            }
        }
        let mut input = std::mem::take(&mut peer.input);
        let local = peer.local;
        let remote = peer.remote;
        self.tcp[index] = Some(peer);
        let mut offset = 0;
        for _ in 0..32 {
            if input.len() - offset < 2 { break; }
            let length = be16(&input, offset) as usize;
            if input.len() - offset - 2 < length { break; }
            offset += 2;
            if length > 0 { self.route(Protocol::Tcp, remote, local, &input[offset..offset + length], Some(index)); }
            offset += length;
        }
        input.copy_within(offset.., 0);
        input.truncate(input.len() - offset);
        if input.len() >= 2 && input.len() - 2 >= be16(&input, 0) as usize { busy = true; }
        if let Some(peer) = self.tcp[index].as_mut() { peer.input = input; }
        if self.tcp[index].is_some() && !self.flush_tcp(index) { self.close_tcp(index); }
        busy
    }

    fn flush_tcp(&mut self, index: usize) -> bool {
        let peer = self.tcp[index].as_mut().unwrap();
        for _ in 0..64 {
            let Some(write) = peer.output.front_mut() else { break; };
            if write.media.is_some_and(|(owner, sequence)| self.sessions[owner].as_ref().and_then(|s| s.sending.as_ref()).is_none_or(|sending| sending.frame.sequence != sequence || sending.failed || Instant::now().duration_since(sending.frame.arrived) >= MAX_AGE)) { return false; }
            let result = if write.offset < 2 {
                peer.stream.write_vectored(&[io::IoSlice::new(&write.header[write.offset..]), io::IoSlice::new(&write.packet)])
            } else { peer.stream.write(&write.packet[write.offset - 2..]) };
            match result {
                Ok(0) => return false,
                Ok(n) => { write.offset += n; peer.bytes -= n; }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(_) => return false,
            }
            if write.offset == write.packet.len() + 2 {
                let write = peer.output.pop_front().unwrap();
                if let Some((owner, sequence)) = write.media {
                    if let Some(sending) = self.sessions[owner].as_mut().and_then(|s| s.sending.as_mut()).filter(|s| s.frame.sequence == sequence) {
                        sending.remaining = sending.remaining.saturating_sub(1);
                    }
                }
            }
        }
        let writable = !peer.output.is_empty();
        if writable == peer.writable { return true; }
        let interest = if writable { Interest::READABLE | Interest::WRITABLE } else { Interest::READABLE };
        let fd = peer.stream.as_raw_fd();
        let registered = self.poll.registry().reregister(&mut SourceFd(&fd), Token(TCP_TOKEN + index), interest).is_ok();
        if registered { peer.writable = writable; }
        registered
    }
}

fn receive_udp(socket: &UdpSocket, bytes: &mut [u8]) -> io::Result<(usize, SocketAddr, SocketAddr)> {
    let mut address: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    let mut control = [0usize; 16];
    let mut vector = libc::iovec { iov_base: bytes.as_mut_ptr().cast(), iov_len: bytes.len() };
    let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
    message.msg_name = (&mut address as *mut libc::sockaddr_in).cast();
    message.msg_namelen = std::mem::size_of_val(&address) as _;
    message.msg_iov = &mut vector; message.msg_iovlen = 1;
    message.msg_control = control.as_mut_ptr().cast(); message.msg_controllen = std::mem::size_of_val(&control) as _;
    let n = unsafe { libc::recvmsg(socket.as_raw_fd(), &mut message, libc::MSG_DONTWAIT) };
    if n < 0 { return Err(io::Error::last_os_error()); }
    if message.msg_flags & (libc::MSG_TRUNC | libc::MSG_CTRUNC) != 0 || address.sin_family as i32 != libc::AF_INET {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "truncated ICE datagram"));
    }
    let mut destination = None;
    let mut header = unsafe { libc::CMSG_FIRSTHDR(&message) };
    while !header.is_null() {
        let h = unsafe { &*header };
        if h.cmsg_level == libc::IPPROTO_IP && h.cmsg_type == libc::IP_PKTINFO && h.cmsg_len as usize >= unsafe { libc::CMSG_LEN(std::mem::size_of::<libc::in_pktinfo>() as _) } as usize {
            let packet = unsafe { &*(libc::CMSG_DATA(header) as *const libc::in_pktinfo) };
            destination = Some(Ipv4Addr::from(packet.ipi_addr.s_addr.to_ne_bytes()));
        }
        header = unsafe { libc::CMSG_NXTHDR(&message, header) };
    }
    let destination = destination.ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing local ICE destination"))?;
    Ok((n as usize,
        SocketAddr::new(IpAddr::V4(Ipv4Addr::from(address.sin_addr.s_addr.to_ne_bytes())), u16::from_be(address.sin_port)),
        SocketAddr::new(IpAddr::V4(destination), socket.local_addr()?.port())))
}

fn send_udp(socket: &UdpSocket, transmit: &Transmit) -> io::Result<usize> {
    let (SocketAddr::V4(source), SocketAddr::V4(destination)) = (transmit.source, transmit.destination) else {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "IPv4 ICE required"));
    };
    let mut address: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    address.sin_family = libc::AF_INET as _; address.sin_port = destination.port().to_be();
    address.sin_addr.s_addr = u32::from_ne_bytes(destination.ip().octets());
    let mut control = [0usize; 8];
    let mut vector = libc::iovec { iov_base: transmit.contents.as_ptr() as *mut libc::c_void, iov_len: transmit.contents.len() };
    let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
    message.msg_name = (&mut address as *mut libc::sockaddr_in).cast(); message.msg_namelen = std::mem::size_of_val(&address) as _;
    message.msg_iov = &mut vector; message.msg_iovlen = 1;
    message.msg_control = control.as_mut_ptr().cast();
    message.msg_controllen = unsafe { libc::CMSG_SPACE(std::mem::size_of::<libc::in_pktinfo>() as _) } as _;
    unsafe {
        let header = libc::CMSG_FIRSTHDR(&message);
        (*header).cmsg_level = libc::IPPROTO_IP; (*header).cmsg_type = libc::IP_PKTINFO;
        (*header).cmsg_len = libc::CMSG_LEN(std::mem::size_of::<libc::in_pktinfo>() as _) as _;
        let packet = libc::CMSG_DATA(header) as *mut libc::in_pktinfo;
        (*packet).ipi_spec_dst.s_addr = u32::from_ne_bytes(source.ip().octets());
    }
    let n = unsafe { libc::sendmsg(socket.as_raw_fd(), &message, libc::MSG_DONTWAIT | libc::MSG_NOSIGNAL) };
    if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
}
