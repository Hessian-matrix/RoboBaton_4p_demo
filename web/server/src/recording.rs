use std::{sync::{Arc, Mutex}, time::{Duration, Instant}};
use bytes::Bytes;
use serde::{Serialize, Deserialize};
use tokio::sync::Notify;
use crate::{ApiError, Service, put_header, request::{decimal, RecordingRequest}};

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordingStatus {
    request_id:String, reply_code:u16, session_id:String, state:String, format:String,
    storage:String, path:String, error:String, outcome:String, data_complete:bool,
    cleanup_complete:bool, elapsed_ms:String, bytes_written:String, dropped_groups:String,
    free_bytes:String,
    #[serde(default,skip_serializing_if="String::is_empty")]
    dropped_frames:String,
    can_start:bool, mp4_available:bool,
    #[serde(default,skip_serializing_if="String::is_empty")]
    health_scope:String,
}
impl RecordingStatus {
    fn valid(&self)->bool {
        [&self.request_id,&self.session_id,&self.elapsed_ms,&self.bytes_written,&self.dropped_groups,&self.free_bytes].iter().all(|v|decimal(v).is_some()) &&
        (self.dropped_frames.is_empty()||decimal(&self.dropped_frames).is_some()) &&
        matches!(self.reply_code,200|202|409|400|503) && matches!(self.state.as_str(),"idle"|"starting"|"recording"|"stopping"|"complete"|"partial"|"failed") &&
        matches!(self.format.as_str(),""|"rosbag"|"mp4") && matches!(self.storage.as_str(),"runtime"|"tf") && self.path.len()<=2048 && self.error.len()<=1024 && self.outcome.len()<=128 && self.health_scope.len()<=128
    }
}
struct Pending { id:String, notify:Notify, reply:Mutex<Option<(RecordingStatus,Instant)>> }
struct Cache { status:Option<(RecordingStatus,Instant)>, pending:Option<Arc<Pending>>, detached:bool }
pub struct Recording {cache:Arc<Mutex<Cache>>}
impl Recording {
    pub fn new()->Self {Self{cache:Arc::new(Mutex::new(Cache{status:None,pending:None,detached:false}))}}
    pub fn invalidate(&self){self.cache.lock().unwrap().status=None;}
    pub fn receive(&self,body:&[u8]) {
        let status:RecordingStatus=match serde_json::from_slice(body){Ok(v)=>v,Err(_)=>return};if !status.valid(){return;}
        let mut cache=self.cache.lock().unwrap();let now=Instant::now();cache.status=Some((status.clone(),now));
        if let Some(pending)=cache.pending.as_ref() {
            if pending.id==status.request_id {
                let mut reply=pending.reply.lock().unwrap();
                if reply.is_none(){*reply=Some((status,now));pending.notify.notify_one();}
                drop(reply);
                if cache.detached {cache.pending=None;}
            }
        }
    }
    pub fn get(&self)->Result<(Bytes,u128),ApiError>{
        let cache=self.cache.lock().unwrap();let (status,at)=cache.status.as_ref().filter(|(_,at)|at.elapsed()<=Duration::from_secs(1)).ok_or_else(unavailable)?;
        Ok((Bytes::from(serde_json::to_vec(status).unwrap()),at.elapsed().as_millis()))
    }
    pub async fn mutate(&self,service:&Service,body:&[u8])->Result<(u16,Bytes,u128),ApiError>{
        let request=RecordingRequest::parse(body).map_err(|m|ApiError::new(400,"recording",m))?;
        let pending;
        {
            let mut cache=self.cache.lock().unwrap();
            let (status,_)=cache.status.as_ref().filter(|(_,at)|at.elapsed()<=Duration::from_secs(1)).ok_or_else(unavailable)?;
            if cache.pending.is_some(){return Err(ApiError::new(409,"recording_pending","a recording request is awaiting acknowledgement; query state, do not retry"));}
            if decimal(&status.session_id)!=Some(request.expected_session_id){return Err(ApiError::new(409,"recording_session","recording session changed; query current state before acting"));}
            if (request.start&&!status.can_start)||(!request.start&&status.state!="recording"){return Err(ApiError::new(409,"recording_state","recording action conflicts with current state; query current state"));}
            if request.start&&request.mp4&&!status.mp4_available{return Err(ApiError::new(400,"recording_format","MP4 recording unavailable; requires H.264 four-camera support"));}
            let mut id=rand::random::<u64>();while id==0||Some(id)==decimal(&status.request_id){id=rand::random();}
            pending=Arc::new(Pending{id:id.to_string(),notify:Notify::new(),reply:Mutex::new(None)});
            cache.pending=Some(pending.clone());cache.detached=false;
        }
        // Cancellation only detaches HTTP; an uncertain producer mutation stays reserved.
        let _guard=PendingGuard{cache:self.cache.clone(),pending:pending.clone()};
        let mut packet=[0u8;64];put_header(&mut packet,0x53575231,service.config.generation);
        packet[16..20].copy_from_slice(&(if request.start{1u32}else{2}).to_be_bytes());
        if request.start {
            for(offset,value) in [(20,if request.mp4{2u32}else{1}),(24,u32::from(request.tf)),(28,request.frame_skip),(32,request.drop_min_interval)]{packet[offset..offset+4].copy_from_slice(&value.to_be_bytes());}
        }
        packet[40..48].copy_from_slice(&decimal(&pending.id).unwrap().to_be_bytes());packet[48..56].copy_from_slice(&request.expected_session_id.to_be_bytes());
        service.write_control(&packet).await.map_err(|_|uncertain("recording command delivery uncertain"))?;
        let wait=async {loop{let notified=pending.notify.notified();if pending.reply.lock().unwrap().is_some(){return;}notified.await;}};
        tokio::time::timeout(Duration::from_secs(3),wait).await.map_err(|_|uncertain("recording acknowledgement timed out"))?;
        let cache=self.cache.lock().unwrap();let reply=pending.reply.lock().unwrap();let(status,at)=reply.as_ref().ok_or_else(||uncertain("recording acknowledgement missing"))?;
        if at.elapsed()>Duration::from_secs(1)||cache.status.as_ref().is_none_or(|(_,t)|t.elapsed()>Duration::from_secs(1)){return Err(uncertain("recording acknowledgement is stale"));}
        if !matches!(status.reply_code,200|202){return Err(ApiError::new(status.reply_code,"recording_rejected",if status.error.is_empty(){"recording command rejected; query current state"}else{&status.error}));}
        Ok((status.reply_code,Bytes::from(serde_json::to_vec(status).unwrap()),at.elapsed().as_millis()))
    }
}
struct PendingGuard {cache:Arc<Mutex<Cache>>,pending:Arc<Pending>}
impl Drop for PendingGuard {
    fn drop(&mut self){let mut cache=self.cache.lock().unwrap();if cache.pending.as_ref().is_some_and(|p|Arc::ptr_eq(p,&self.pending)){cache.detached=true;if self.pending.reply.lock().unwrap().is_some(){cache.pending=None;}}}
}
fn unavailable()->ApiError{ApiError::new(503,"recording_unavailable","recording status unavailable or stale; controls disabled")}
fn uncertain(message:&str)->ApiError{ApiError::new(503,"recording_uncertain",format!("{message}; query /api/v1/recording before acting; do not blindly retry"))}
