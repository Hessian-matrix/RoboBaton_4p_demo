use std::{convert::Infallible, pin::Pin, net::SocketAddr, sync::{Arc, atomic::Ordering}, task::{Context, Poll}, time::Duration};
use bytes::Bytes;
use http_body_util::{BodyExt, Full, combinators::UnsyncBoxBody};
use hyper::{Request, Response, Method, StatusCode, body::{Incoming, Frame, Body}, service::service_fn};
use hyper_util::rt::{TokioIo, TokioTimer};
use serde_json::json;
use tokio::net::TcpListener;
use crate::{Service, ApiError, VERSION, WEBRTC_VERSION, transport::WriteDeadline};

type ResponseBody=UnsyncBoxBody<Bytes,Infallible>;
struct Events(crate::telemetry::Events);
impl Body for Events {
    type Data=Bytes;type Error=Infallible;
    fn poll_frame(mut self:Pin<&mut Self>,cx:&mut Context<'_>)->Poll<Option<Result<Frame<Bytes>,Infallible>>>{self.0.poll(cx).map(|b|b.map(|b|Ok(Frame::data(b))))}
}
fn response(status:u16,content_type:&str,body:impl Into<Bytes>)->Response<ResponseBody>{
    let mut response=Response::new(Full::new(body.into()).boxed_unsync());*response.status_mut()=StatusCode::from_u16(status).unwrap();
    response.headers_mut().insert("Content-Type",content_type.parse().unwrap());response
}
fn json_response(value:serde_json::Value)->Response<ResponseBody>{response(200,"application/json",serde_json::to_vec(&value).unwrap())}
fn error(e:ApiError)->Response<ResponseBody>{
    let mut detail=json!({"code":e.code,"message":e.message});if let Some(profile)=e.source_profile{detail["source_profile_level_id"]=profile.into();}
    response(e.status,"application/json",serde_json::to_vec(&json!({"error":detail})).unwrap())
}
fn text_header<'a>(req:&'a Request<Incoming>,name:&str)->Option<&'a str>{req.headers().get(name).and_then(|v|v.to_str().ok())}
fn validate(req:&Request<Incoming>,local:SocketAddr,port:u16)->Result<(),ApiError>{
    let bad_host=||ApiError::new(400,"host","invalid host or query");
    let host=text_header(req,"host").ok_or_else(bad_host)?;
    if !local.is_ipv4()||local.ip().is_unspecified()||local.port()!=port||req.uri().query().is_some(){return Err(bad_host());}
    let ip=local.ip().to_string();let with_port=format!("{ip}:{port}");if host!=ip && host!=with_port {return Err(bad_host());}
    if let Some(origin)=text_header(req,"origin"){if origin!=format!("http://{host}"){return Err(ApiError::new(400,"origin","same origin required"));}}
    if text_header(req,"sec-fetch-site")==Some("cross-site"){return Err(ApiError::new(400,"origin","same origin required"));}
    Ok(())
}
async fn read_body(req:Request<Incoming>,limit:usize)->Result<Bytes,ApiError>{
    let read=async {let mut body=req.into_body();let mut out=Vec::new();while let Some(frame)=body.frame().await{let frame=frame.map_err(|_|ApiError::new(400,"body","request body unavailable"))?;if let Ok(data)=frame.into_data(){if data.len()>limit.saturating_sub(out.len()){return Err(ApiError::new(400,"body","request body exceeds limit"));}out.extend_from_slice(&data);}}Ok(Bytes::from(out))};
    tokio::time::timeout(Duration::from_secs(15),read).await.map_err(|_|ApiError::new(400,"body","request body timeout"))?
}
async fn route(s:Arc<Service>,req:Request<Incoming>,local:SocketAddr)->Result<Response<ResponseBody>,ApiError>{
    validate(&req,local,s.config.port)?;
    let method=req.method().clone();let path=req.uri().path();
    let require=|expected:&str|->Result<(),ApiError>{if method.as_str()==expected{Ok(())}else{Err(ApiError::new(405,"method",format!("{expected} required")))}};
    match path {
        "/api/v1/info"=>{require("GET")?;let cameras:Vec<_>=(0..4).map(|i|json!({"camera_id":i,"enabled":s.config.camera_mask&(1<<i)!=0,"sessions":format!("/api/v1/cameras/{i}/sessions")})).collect();Ok(json_response(json!({"application_session_id":s.config.generation.to_string(),"version":VERSION,"web_implementation":"rust","webrtc_stack":"str0m","webrtc_version":WEBRTC_VERSION,"camera_mask":s.config.camera_mask,"width":s.config.width,"height":s.config.height,"fps":s.config.fps,"bitrate_bps":s.config.bitrate_bps,"codec":s.config.codec,"web_enabled":true,"rtsp_enabled":s.config.rtsp_enabled,"cameras":cameras,"source_rate_hz":s.config.source_rate_hz,"telemetry_limit_hz":s.config.telemetry_limit_hz,"accel_unit":"m/s²","gyro_unit":"rad/s"})))},
        "/api/v1/status"=>{require("GET")?;let bits=s.source_bits.load(Ordering::Relaxed);let at=s.source_at.load(Ordering::Acquire);let stale=at==0||s.now().saturating_sub(at)>500_000_000;
            let mut result=json!({"application_session_id":s.config.generation.to_string(),"telemetry":s.telemetry.state(),"rtsp":if !s.config.rtsp_enabled{"disabled"}else if stale{"stale"}else if bits&16!=0{"degraded"}else{"enabled"}});
            for(i,key)in ["capture","encoding","saving"].into_iter().enumerate(){result[key]=(if stale{"stale"}else if bits&8!=0{"degraded"}else if bits&(1<<i)!=0{"flowing"}else{"idle"}).into();}
            result["cameras"]=s.media.cameras().into_iter().map(|c|json!({"camera_id":c.camera_id,"web":c.web,"ipc_drops":c.ipc_drops.saturating_add(s.video_drops[c.camera_id].load(Ordering::Relaxed)).to_string()})).collect::<Vec<_>>().into();Ok(json_response(result))},
        "/api/v1/imu/events"=>{require("GET")?;let events=s.telemetry.subscribe()?;let mut r=Response::new(Events(events).boxed_unsync());r.headers_mut().insert("Content-Type","text/event-stream".parse().unwrap());r.headers_mut().insert("X-Accel-Buffering","no".parse().unwrap());Ok(r)},
        "/api/v1/recording"=>{
            if method==Method::GET{let(body,age)=s.recording.get()?;let mut r=response(200,"application/json",body);r.headers_mut().insert("X-Recording-Status-Age-Ms",age.to_string().parse().unwrap());return Ok(r);}
            if method!=Method::POST{return Err(ApiError::new(405,"method","GET or POST required"));}
            let origin=format!("http://{}",text_header(&req,"host").unwrap_or(""));if text_header(&req,"origin")!=Some(&origin){return Err(ApiError::new(400,"origin","exact same Origin required for recording control"));}
            if text_header(&req,"content-type")!=Some("application/json"){return Err(ApiError::new(400,"body","application/json required"));}
            let body=read_body(req,1024).await?;let(code,body,age)=s.recording.mutate(&s,&body).await?;let mut r=response(code,"application/json",body);r.headers_mut().insert("X-Recording-Status-Age-Ms",age.to_string().parse().unwrap());Ok(r)
        },
        _ if path.starts_with("/api/v1/cameras/")=>{
            require("POST")?;let parts:Vec<_>=path.split('/').collect();if parts.len()!=6||parts[5]!="sessions"{return Err(ApiError::new(404,"resource","unknown camera"));}
            let camera=parts[4].parse::<usize>().ok().filter(|i|*i<4&&i.to_string()==parts[4]&&s.config.camera_mask&(1<<i)!=0).ok_or_else(||ApiError::new(404,"camera","camera is not enabled"))?;
            if s.config.codec!="h264"{return Err(ApiError::new(422,"codec","Web video supports H.264 only"));}
            if text_header(&req,"content-type")!=Some("application/json"){return Err(ApiError::new(400,"body","JSON offer required"));}
            #[derive(serde::Deserialize)]#[serde(deny_unknown_fields)]struct Offer{#[serde(rename="type")]kind:String,sdp:String}
            let body=read_body(req,96*1024).await?;let offer:Offer=serde_json::from_slice(&body).map_err(|_|ApiError::new(400,"offer","one receive-only video offer required"))?;
            if offer.kind!="offer"{return Err(ApiError::new(400,"offer","one receive-only video offer required"));}
            let answer=s.media.create(camera,offer.sdp).await?;let location=format!("/api/v1/video-sessions/{}",answer.session_id);let mut r=response(201,"application/json",serde_json::to_vec(&answer).unwrap());r.headers_mut().insert("Location",location.parse().unwrap());Ok(r)
        },
        _ if path.starts_with("/api/v1/video-sessions/")=>{require("DELETE")?;let id=path.trim_start_matches("/api/v1/video-sessions/");if id.len()!=32||!id.bytes().all(|c|c.is_ascii_digit()||(b'a'..=b'f').contains(&c)){return Err(ApiError::new(404,"session","unknown session"));}s.media.delete(id);Ok(response(204,"application/json",Bytes::new()))},
        _=>{let asset=match path{
            "/"=>Some(("text/html; charset=utf-8",include_bytes!("../assets/index.html").as_slice())),
            "/app.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/app.js").as_slice())),
            "/style.css"=>Some(("text/css; charset=utf-8",include_bytes!("../assets/style.css").as_slice())),
            "/i18n.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/i18n.js").as_slice())),
            "/dashboard.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/dashboard.js").as_slice())),
            "/attitude.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/attitude.js").as_slice())),
            "/attitude-worker.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/attitude-worker.js").as_slice())),
            "/recording.js"=>Some(("text/javascript; charset=utf-8",include_bytes!("../assets/recording.js").as_slice())),_=>None};
            let(mime,body)=asset.ok_or_else(||ApiError::new(404,"resource","not found"))?;require("GET")?;Ok(response(200,mime,Bytes::from_static(body)))
        }
    }
}
async fn handle(s:Arc<Service>,req:Request<Incoming>,local:SocketAddr)->Result<Response<ResponseBody>,Infallible>{
    let mut response=route(s,req,local).await.unwrap_or_else(error);
    let h=response.headers_mut();
    h.insert("X-Content-Type-Options","nosniff".parse().unwrap());h.insert("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; media-src 'self' blob:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'".parse().unwrap());h.insert("X-Frame-Options","DENY".parse().unwrap());h.insert("Cache-Control","no-store".parse().unwrap());
    Ok(response)
}
pub async fn serve(s:Arc<Service>,listener:TcpListener){
    let mut stop=s.stop.subscribe();let mut connections=tokio::task::JoinSet::new();
    let mut accept_delay = Duration::from_millis(5);
    loop{
        tokio::select!{
            _=stop.changed()=>break,
            Some(_)=connections.join_next(),if !connections.is_empty()=>{},
            accepted=listener.accept()=>{
                let (stream, _) = match accepted {
                    Ok(value) => { accept_delay = Duration::from_millis(5); value }
                    Err(_) => {
                        tokio::select! { _ = stop.changed() => break, _ = tokio::time::sleep(accept_delay) => {} }
                        accept_delay = (accept_delay * 2).min(Duration::from_secs(1));
                        continue;
                    }
                };
                let local=match stream.local_addr(){Ok(v)=>v,Err(_)=>continue};let _=stream.set_nodelay(true);let s=s.clone();
                connections.spawn(async move{let mut builder=hyper::server::conn::http1::Builder::new();builder.timer(TokioTimer::new()).header_read_timeout(Duration::from_secs(3)).max_headers(64).max_header_size(16*1024).max_buf_size(32*1024);let io=TokioIo::new(WriteDeadline::new(stream));let _=builder.serve_connection(io,service_fn(move |r|handle(s.clone(),r,local))).await;});
            }
        }
    }
    connections.abort_all();while connections.join_next().await.is_some(){}
}
