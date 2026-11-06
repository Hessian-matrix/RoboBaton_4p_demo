mod http;
mod media;
mod negotiation;
mod recording;
mod request;
mod telemetry;
mod transport;

include!(concat!(env!("OUT_DIR"), "/identity.rs"));

use std::{fs::File, io::{self, Read}, net::{TcpListener, UdpSocket}, os::fd::{AsRawFd, FromRawFd, OwnedFd}, os::unix::net::UnixDatagram, sync::{Arc, atomic::{AtomicU32, AtomicU64, Ordering}}, time::{Duration, Instant}};
use serde::Deserialize;
use tokio::{io::unix::AsyncFd, sync::watch};

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    schema_version: u32,
    #[serde(deserialize_with = "generation")]
    generation: u64,
    camera_mask: u32,
    width: u32, height: u32, fps: u32, bitrate_bps: u64,
    codec: String, rtsp_enabled: bool, web_enabled: bool,
    listen_address: String, port: u16, ice_port: u16,
    source_rate_hz: u32, telemetry_limit_hz: u32,
    max_video_sessions: usize, max_imu_clients: usize,
}
fn generation<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u64,D::Error> {
    let s = String::deserialize(d)?;
    request::decimal(&s).ok_or_else(|| serde::de::Error::custom("generation must be decimal uint64"))
}

#[derive(Debug)]
pub struct ApiError { pub status: u16, pub code: &'static str, pub message: String, pub source_profile: Option<String> }
impl ApiError {
    pub fn new(status: u16, code: &'static str, message: impl Into<String>) -> Self { Self { status, code, message: message.into(), source_profile: None } }
}

pub struct Service {
    config: Arc<Config>, media: media::MediaHandle, telemetry: Arc<telemetry::Telemetry>,
    control: Arc<tokio::net::UnixDatagram>, recording: recording::Recording,
    source_bits: AtomicU32, source_at: AtomicU64, video_drops: [AtomicU64;4],
    epoch: Instant, stop: watch::Sender<bool>,
}
impl Service {
    fn now(&self) -> u64 { self.epoch.elapsed().as_nanos().min(u64::MAX as u128 - 1) as u64 + 1 }
    async fn write_control(&self, data: &[u8]) -> io::Result<()> {
        let n = tokio::time::timeout(Duration::from_millis(20),self.control.send(data)).await.map_err(|_| io::Error::new(io::ErrorKind::TimedOut,"control write timeout"))??;
        if n != data.len() { return Err(io::Error::new(io::ErrorKind::WriteZero,"short datagram write")); }
        Ok(())
    }
    async fn control_read(self: Arc<Self>) {
        let mut buffer=[0u8;4097];let mut stop=self.stop.subscribe();
        loop {
            let result=tokio::select! { r=self.control.recv(&mut buffer)=>r, _=stop.changed()=>break };
            let n=match result { Ok(n)=>n,Err(_)=>break };let b=&buffer[..n];
            if n>16 && n<=4096 && header(b,0x53575232,n,self.config.generation) {
                self.recording.receive(&b[16..]);
            } else if n==64 && header(b,0x53575331,64,self.config.generation) {
                self.source_bits.store(be32(&b[16..]),Ordering::Relaxed);self.source_at.store(self.now(),Ordering::Release);
                for i in 0..4 {self.video_drops[i].store(be64(&b[32+i*8..]),Ordering::Relaxed);}
            }
        }
        self.recording.invalidate();
    }
    async fn heartbeat(self: Arc<Self>) {
        let mut timer=tokio::time::interval(Duration::from_millis(100));timer.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut stop=self.stop.subscribe();let mut b=[0u8;32];put_header(&mut b,0x53574331,self.config.generation);
        loop {
            tokio::select! {_=timer.tick()=>{},_=stop.changed()=>break}
            let mask=self.media.demand() | if self.telemetry.clients()>0 {16} else {0};
            b[24..28].copy_from_slice(&mask.to_be_bytes());let _=self.write_control(&b).await;
        }
    }
}
pub fn be32(b:&[u8])->u32 {u32::from_be_bytes(b[..4].try_into().unwrap())}
pub fn be64(b:&[u8])->u64 {u64::from_be_bytes(b[..8].try_into().unwrap())}
pub fn header(b:&[u8],magic:u32,n:usize,generation:u64)->bool {b.len()>=16 && be32(b)==magic && b[4..6]==[0,1] && u16::from_be_bytes([b[6],b[7]]) as usize==n && be64(&b[8..])==generation}
pub fn put_header(b:&mut[u8],magic:u32,generation:u64) {let n=b.len() as u16;b[..4].copy_from_slice(&magic.to_be_bytes());b[4..6].copy_from_slice(&1u16.to_be_bytes());b[6..8].copy_from_slice(&n.to_be_bytes());b[8..16].copy_from_slice(&generation.to_be_bytes());}

fn main() {
    // These loaded bytes also back the reported version, compiler and source digest.
    // Comparing the complete records binds the dependency graph at every entrypoint.
    if std::hint::black_box(&BUILD_IDENTITY) != std::hint::black_box(&RUNTIME_BUILD_IDENTITY) {
        eprintln!("sensor_web build identity does not match runtime identity");
        std::process::exit(1);
    }
    let args:Vec<_>=std::env::args().collect();
    if args.len()==2 && args[1]=="--version" {println!("sensor_web {VERSION} Rust {COMPILER} str0m {WEBRTC_VERSION} source-sha256={SOURCE_DIGEST}");return;}
    if args.len()!=2 || args[1]!="--inherited-fds" {eprintln!("sensor_web is owned by sensor_demo; use sensor_demo --web");std::process::exit(1);}
    if let Err(e)=run() {eprintln!("web=degraded: {e}");std::process::exit(1);}
}
fn run()->Result<(),Box<dyn std::error::Error>> {
    // FD ownership transfers from the parent's exec contract exactly once.
    let mut file=unsafe{File::from_raw_fd(3)};let mut bytes=Vec::with_capacity(4096);file.by_ref().take(4097).read_to_end(&mut bytes)?;drop(file);
    if bytes.len()>4096 {return Err("inherited configuration exceeds 4096 bytes".into());}
    let c:Config=serde_json::from_slice(&bytes)?;
    if c.schema_version!=1 || c.generation==0 || !(1..=15).contains(&c.camera_mask) || c.listen_address.parse::<std::net::Ipv4Addr>().is_err() || c.port==0 || c.ice_port!=8189 || !(1..=8).contains(&c.max_video_sessions) || !(1..=2).contains(&c.max_imu_clients) || !(1..=50).contains(&c.telemetry_limit_hz) || !c.web_enabled {return Err("invalid inherited configuration".into());}
    let c=Arc::new(c);
    let video=unsafe{UnixDatagram::from_raw_fd(4)};let imu=unsafe{UnixDatagram::from_raw_fd(5)};let control=unsafe{UnixDatagram::from_raw_fd(6)};let life=unsafe{OwnedFd::from_raw_fd(7)};
    let http=TcpListener::bind((c.listen_address.as_str(),c.port))?;
    let udp=UdpSocket::bind((c.listen_address.as_str(),c.ice_port))?;let tcp=TcpListener::bind((c.listen_address.as_str(),c.ice_port))?;
    http.set_nonblocking(true)?;imu.set_nonblocking(true)?;control.set_nonblocking(true)?;
    let flags=unsafe{libc::fcntl(life.as_raw_fd(),libc::F_GETFL)};
    if flags<0 || unsafe{libc::fcntl(life.as_raw_fd(),libc::F_SETFL,flags|libc::O_NONBLOCK)}<0 {return Err(io::Error::last_os_error().into());}
    // Privileged binds precede credential drop and all worker thread creation.
    if unsafe{libc::geteuid()}==0 {
        if unsafe{libc::setgroups(0,std::ptr::null())}!=0 || unsafe{libc::setgid(65534)}!=0 || unsafe{libc::setuid(65534)}!=0 {return Err(io::Error::last_os_error().into());}
    }
    let runtime=tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build()?;
    runtime.block_on(async move {
        let media=media::MediaHandle::spawn(c.clone(),video,udp,tcp)?;
        let telemetry=Arc::new(telemetry::Telemetry::new(c.clone()));
        let (stop,_)=watch::channel(false);
        let s=Arc::new(Service {config:c,media,telemetry,control:Arc::new(tokio::net::UnixDatagram::from_std(control)?),recording:recording::Recording::new(),source_bits:AtomicU32::new(0),source_at:AtomicU64::new(0),video_drops:std::array::from_fn(|_|AtomicU64::new(0)),epoch:Instant::now(),stop});
        let tasks=[tokio::spawn(s.clone().control_read()),tokio::spawn(s.clone().heartbeat()),tokio::spawn(s.telemetry.clone().read(tokio::net::UnixDatagram::from_std(imu)?,s.stop.subscribe()))];
        let listener=tokio::net::TcpListener::from_std(http)?;
        let mut http_task=tokio::spawn(http::serve(s.clone(),listener));
        // OwnedFd keeps this nonblocking descriptor valid until deregistration.
        let life=unsafe{AsyncFd::register(life)}?;
        let parent_gone=async {loop {let mut ready=life.readable().await?;let mut b=[0u8;1];match ready.try_io(|fd| {let n=unsafe{libc::read(fd.as_raw_fd(),b.as_mut_ptr().cast(),1)};if n<0 {Err(io::Error::last_os_error())}else{Ok(n)}}) {Ok(_)=>return Ok::<(),io::Error>(()),Err(_)=>continue}}};
        let mut term=tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
        let mut interrupt=tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;
        tokio::select! {_=parent_gone=>{},_=term.recv()=>{},_=interrupt.recv()=>{},_=&mut http_task=>{}}
        let _=s.stop.send(true);s.media.shutdown().await;
        http_task.abort();for task in tasks {task.abort();}
        Ok::<(),Box<dyn std::error::Error>>(())
    })
}
