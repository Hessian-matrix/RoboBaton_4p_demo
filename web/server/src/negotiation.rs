use crate::ApiError;
use std::collections::HashMap;
use str0m::change::SdpOffer;

pub struct Selection {
    pub offer: SdpOffer,
    pub payload: u8,
    pub resend: Option<u8>,
    pub profile: u32,
    pub nack: bool,
    pub pli: bool,
    pub fir: bool,
}

fn offer_error() -> ApiError {
    ApiError::new(400, "offer", "one receive-only video offer required")
}

/// Validate direction and media count before reserving a source reader.
pub fn valid_offer(text: &str) -> bool {
    if text.len() > 64 * 1024 || text.contains('\0') || SdpOffer::from_sdp_string(text).is_err() {
        return false;
    }
    let mut media = false;
    let mut receive = false;
    let mut session_receive = false;
    for line in text.lines().map(|l| l.trim_end_matches('\r')) {
        if let Some(value) = line.strip_prefix("m=") {
            if media { return false; }
            let mut fields = value.split_ascii_whitespace();
            if fields.next() != Some("video") || fields.next().and_then(|p| p.split('/').next()?.parse::<u16>().ok()).is_none_or(|p| p == 0) {
                return false;
            }
            media = true;
            receive = session_receive;
        } else if line == "a=recvonly" {
            if media { receive = true; } else { session_receive = true; }
        } else if media && matches!(line, "a=sendrecv" | "a=sendonly" | "a=inactive") {
            return false;
        }
    }
    media && receive
}

pub fn profile_id(value: &str) -> Option<[u8; 3]> {
    if value.len() != 6 || !value.is_ascii() { return None; }
    Some([
        u8::from_str_radix(&value[0..2], 16).ok()?,
        u8::from_str_radix(&value[2..4], 16).ok()?,
        u8::from_str_radix(&value[4..6], 16).ok()?,
    ])
}

fn subprofile(p: [u8; 3]) -> u32 {
    match p[0] {
        0x42 if p[1] & 0x4f == 0x40 => 1,
        0x42 if p[1] & 0x4f == 0 => 2,
        0x4d if p[1] & 0x8f == 0x80 => 1,
        0x4d if p[1] & 0xaf == 0 => 3,
        0x58 if p[1] & 0xcf == 0xc0 => 1,
        0x58 if p[1] & 0xcf == 0x80 => 2,
        0x58 if p[1] & 0xcf == 0 => 4,
        _ => 0x10000 | u16::from_be_bytes([p[0], p[1]]) as u32,
    }
}

fn level(p: [u8; 3]) -> u16 {
    if matches!(p[0], 0x42 | 0x4d | 0x58) && p[2] == 11 && p[1] & 0x10 != 0 { 105 } else { p[2] as u16 * 10 }
}

pub fn supports(source: &str, remote: &str) -> bool {
    match (profile_id(source), profile_id(remote)) {
        (Some(s), Some(r)) => subprofile(s) == subprofile(r) && level(s) <= level(r),
        _ => false,
    }
}

/// Only the chosen source-compatible format reaches str0m. Normalize the peer's
/// receive ceiling to the actual SPS so the answer never overstates the stream.
pub fn select(text: &str, source: &str) -> Result<Selection, ApiError> {
    if !valid_offer(text) { return Err(offer_error()); }
    let actual = profile_id(source).ok_or_else(|| ApiError::new(503, "source", "source parameter sets unavailable"))?;
    let mut maps = HashMap::new();
    let mut formats = HashMap::new();
    let mut order = Vec::new();
    let mut in_media = false;
    for line in text.lines().map(|l| l.trim_end_matches('\r')) {
        if let Some(m) = line.strip_prefix("m=") {
            in_media = true;
            order.extend(m.split_ascii_whitespace().skip(3).filter_map(|v| v.parse::<u8>().ok()).filter(|v| *v <= 127));
        }
        if !in_media { continue; }
        if let Some(v) = line.strip_prefix("a=rtpmap:") {
            if let Some((pt, spec)) = v.split_once(' ') { maps.insert(pt.to_owned(), spec.trim().to_owned()); }
        }
        if let Some(v) = line.strip_prefix("a=fmtp:") {
            if let Some((pt, params)) = v.split_once(' ') { formats.insert(pt.to_owned(), params.trim().to_owned()); }
        }
    }
    let selected = order.iter().copied().find(|pt| {
        let key = pt.to_string();
        if !maps.get(&key).is_some_and(|v| v.eq_ignore_ascii_case("H264/90000")) { return false; }
        let params = parameters(formats.get(&key).map(String::as_str).unwrap_or(""));
        params.get("packetization-mode") == Some(&"1") && params.get("profile-level-id").is_some_and(|p| supports(source, p))
    });
    let payload = selected.ok_or_else(|| {
        let mut e = ApiError::new(422, "negotiation", "receiver does not support source H.264 profile/level and packetization mode");
        e.source_profile = Some(source.to_owned());
        e
    })?;
    let key = payload.to_string();
    let resend = order.iter().copied().find(|pt| {
        let k = pt.to_string();
        maps.get(&k).is_some_and(|v| v.eq_ignore_ascii_case("rtx/90000")) && parameters(formats.get(&k).map(String::as_str).unwrap_or("")).get("apt").is_some_and(|v| *v == key)
    });
    let mut nack = false;
    let mut pli = false;
    let mut fir = false;
    let mut normalized = String::with_capacity(text.len());
    for line in text.lines().map(|l| l.trim_end_matches('\r')) {
        if line.starts_with("m=") {
            let mut words = line.split_ascii_whitespace();
            let header = words.by_ref().take(3).collect::<Vec<_>>().join(" ");
            normalized.push_str(&format!("{header} {payload}"));
            if let Some(rtx) = resend { normalized.push_str(&format!(" {rtx}")); }
            normalized.push_str("\r\n");
            continue;
        }
        if let Some(value) = line.strip_prefix("a=rtpmap:").or_else(|| line.strip_prefix("a=fmtp:")).or_else(|| line.strip_prefix("a=rtcp-fb:")) {
            let pt = value.split_ascii_whitespace().next().unwrap_or("");
            if pt != key && pt != "*" && !resend.is_some_and(|r| pt.parse::<u8>() == Ok(r)) { continue; }
            if line.starts_with("a=fmtp:") && pt == key {
                normalized.push_str(&format!("a=fmtp:{payload} level-asymmetry-allowed=0;packetization-mode=1;profile-level-id={source}\r\n"));
                continue;
            }
            if line.starts_with("a=rtcp-fb:") {
                let fields: Vec<_> = value.split_ascii_whitespace().collect();
                match fields.get(1..).unwrap_or(&[]) {
                    ["nack"] => nack = true,
                    ["nack", "pli"] => pli = true,
                    ["ccm", "fir"] => fir = true,
                    _ => continue,
                }
            }
        }
        normalized.push_str(line);
        normalized.push_str("\r\n");
    }
    let offer = SdpOffer::from_sdp_string(&normalized).map_err(|_| offer_error())?;
    Ok(Selection { offer, payload, resend, profile: u32::from_be_bytes([0, actual[0], actual[1], actual[2]]), nack, pli, fir })
}

fn parameters(value: &str) -> HashMap<&str, &str> {
    value.split(';').filter_map(|part| part.trim().split_once('=')).map(|(k,v)| (k.trim(), v.trim())).collect()
}
