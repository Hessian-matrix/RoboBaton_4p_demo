use std::{sync::{Arc, Mutex, atomic::{AtomicUsize, Ordering}}, task::{Context, Poll, Waker}, pin::Pin, time::{Duration, Instant}};
use bytes::Bytes;
use serde::Serialize;
use tokio::{sync::watch, time::Sleep};
use crate::{Config, ApiError, be32, be64, header};

struct Client { slot: Mutex<Slot> }
struct Slot { body: Option<Bytes>, overwrites: u64, waker: Option<Waker> }
impl Client {
    fn put(&self, body: Bytes) {
        let mut slot=self.slot.lock().unwrap();
        if slot.body.replace(body).is_some(){slot.overwrites=slot.overwrites.saturating_add(1);}
        let waker=slot.waker.take();drop(slot);if let Some(waker)=waker{waker.wake();}
    }
}
pub struct Telemetry {config:Arc<Config>,count:AtomicUsize,last:Mutex<Option<Instant>>,subscribers:Mutex<Vec<Arc<Client>>>}
struct Decimal(u64);
impl Serialize for Decimal {
    fn serialize<S:serde::Serializer>(&self,serializer:S)->Result<S::Ok,S::Error>{serializer.collect_str(&self.0)}
}
#[derive(Serialize)]
struct Sample {
    schema_version:u32,session_id:Decimal,sample_sequence:Decimal,sample_realtime_ns:Decimal,
    observation_monotonic_ns:Decimal,temperature_c:Option<f64>,accel_mps2:[Option<f64>;3],gyro_rps:[Option<f64>;3],
    attitude_accel_mps2:[Option<f64>;3],attitude_gyro_rps:[Option<f64>;3],source_rate_hz:u32,telemetry_limit_hz:u32,
    timestamp_uncertainty_ns:Option<u32>,timestamp_mode:&'static str,source_generation:u32,burst_flags:u32,
    evidence_id:Decimal,telemetry_counters:Counters,sample_monotonic_ns:Decimal,sample_raw_ns:Decimal,
    burst_complete_raw_ns:Decimal,estimated_period_ns:Decimal,bridge_monotonic_ns:Decimal,bridge_raw_ns:Decimal,
    bridge_realtime_ns:Decimal,bridge_uncertainty_ns:Decimal,timestamp_source:u32,packet_flags:u32,fifo_tmst:u32,
}
#[derive(Serialize)]
struct Counters {ipc_send_drops:Decimal}
impl Telemetry {
    pub fn new(config:Arc<Config>)->Self{Self{config,count:AtomicUsize::new(0),last:Mutex::new(None),subscribers:Mutex::new(Vec::new())}}
    pub fn clients(&self)->usize{self.count.load(Ordering::Relaxed)}
    pub fn state(&self)->&'static str {match *self.last.lock().unwrap(){None=>"idle",Some(t) if t.elapsed()>Duration::from_millis(500)=>"stale",Some(_)=>"flowing"}}
    pub async fn read(self:Arc<Self>,socket:tokio::net::UnixDatagram,mut stop:watch::Receiver<bool>){
        let mut buffer=[0u8;257];
        loop {
            let n=tokio::select!{r=socket.recv(&mut buffer)=>match r{Ok(n)=>n,Err(_)=>break},_=stop.changed()=>break};
            let b=&buffer[..n];if n!=256 || !header(b,0x53574932,256,self.config.generation){continue;}
            *self.last.lock().unwrap()=Some(Instant::now());
            if self.clients()==0{continue;}
            let f=|offset:usize|{let v=f64::from_bits(be64(&b[offset..]));if v.is_finite(){Some(v)}else{None}};
            let vector=|offset:usize|[f(offset),f(offset+8),f(offset+16)];
            let decimal=|offset:usize|Decimal(be64(&b[offset..]));
            let tmst=be32(&b[108..])==1;
            let sample=Sample{schema_version:2,session_id:Decimal(self.config.generation),sample_sequence:decimal(16),
                sample_realtime_ns:decimal(24),observation_monotonic_ns:decimal(32),temperature_c:f(40),accel_mps2:vector(48),gyro_rps:vector(72),
                attitude_accel_mps2:vector(128),attitude_gyro_rps:vector(152),source_rate_hz:self.config.source_rate_hz,telemetry_limit_hz:self.config.telemetry_limit_hz,
                timestamp_uncertainty_ns:if tmst{Some(be32(&b[96..]))}else{None},timestamp_mode:if tmst{"tmst"}else{"host"},source_generation:be32(&b[100..]),
                burst_flags:be32(&b[104..]),evidence_id:decimal(120),telemetry_counters:Counters{ipc_send_drops:decimal(112)},sample_monotonic_ns:decimal(176),
                sample_raw_ns:decimal(184),burst_complete_raw_ns:decimal(192),estimated_period_ns:decimal(200),bridge_monotonic_ns:decimal(208),
                bridge_raw_ns:decimal(216),bridge_realtime_ns:decimal(224),bridge_uncertainty_ns:decimal(232),timestamp_source:be32(&b[240..]),packet_flags:be32(&b[244..]),fifo_tmst:be32(&b[248..])};
            let mut event=Vec::with_capacity(1400);event.extend_from_slice(b"event: imu\ndata: ");
            if serde_json::to_writer(&mut event,&sample).is_ok(){event.extend_from_slice(b"\n\n");let event=Bytes::from(event);for client in self.subscribers.lock().unwrap().iter(){client.put(event.clone());}}
        }
    }
    pub fn subscribe(self:&Arc<Self>)->Result<Events,ApiError>{
        if self.count.fetch_add(1,Ordering::AcqRel)>=self.config.max_imu_clients{self.count.fetch_sub(1,Ordering::AcqRel);return Err(ApiError::new(429,"limit","IMU reader limit reached"));}
        let client=Arc::new(Client{slot:Mutex::new(Slot{body:None,overwrites:0,waker:None})});
        self.subscribers.lock().unwrap().push(client.clone());
        Ok(Events{hub:self.clone(),client,initial:true,timer:Box::pin(tokio::time::sleep(Duration::from_secs(1)))})
    }
}
pub struct Events {hub:Arc<Telemetry>,client:Arc<Client>,initial:bool,timer:Pin<Box<Sleep>>}
impl Events {
    pub fn poll(&mut self,cx:&mut Context<'_>)->Poll<Option<Bytes>>{
        if self.initial{self.initial=false;return Poll::Ready(Some(Bytes::from_static(b"event: status\ndata: {\"state\":\"waiting\"}\n\n")));}
        let status_due=self.timer.as_mut().poll(cx).is_ready();
        let mut slot=self.client.slot.lock().unwrap();
        let overwrites=slot.overwrites;
        // Register and inspect under one lock so producer notification cannot be lost.
        slot.waker=Some(cx.waker().clone());
        let body=if status_due{None}else{slot.body.take()};drop(slot);
        if status_due{
            self.timer.as_mut().reset(tokio::time::Instant::now()+Duration::from_secs(1));
            return Poll::Ready(Some(Bytes::from(format!("event: status\ndata: {{\"state\":\"{}\",\"client_overwrites\":\"{}\"}}\n\n",self.hub.state(),overwrites))));
        }
        match body {Some(body)=>Poll::Ready(Some(body)),None=>Poll::Pending}
    }
}
impl Drop for Events {
    fn drop(&mut self){self.hub.subscribers.lock().unwrap().retain(|s|!Arc::ptr_eq(s,&self.client));self.hub.count.fetch_sub(1,Ordering::AcqRel);}
}
