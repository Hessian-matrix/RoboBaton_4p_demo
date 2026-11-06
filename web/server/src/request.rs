use serde::Deserialize;
use serde_json::Value;

pub fn decimal(value: &str) -> Option<u64> {
    if value.is_empty() || (value.len()>1 && value.starts_with('0')) || !value.bytes().all(|b|b.is_ascii_digit()) { return None; }
    value.parse::<u64>().ok()
}

fn present<'de,D:serde::Deserializer<'de>>(deserializer:D)->Result<Option<Value>,D::Error>{Value::deserialize(deserializer).map(Some)}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawRequest {
    action: String,
    expected_session_id: String,
    #[serde(default,deserialize_with="present")]
    format: Option<Value>,
    #[serde(default,deserialize_with="present")]
    storage: Option<Value>,
    #[serde(default,deserialize_with="present")]
    frame_skip: Option<Value>,
    #[serde(default,deserialize_with="present")]
    drop_min_interval: Option<Value>,
}

pub struct RecordingRequest {
    pub start: bool,
    pub expected_session_id: u64,
    pub mp4: bool,
    pub tf: bool,
    pub frame_skip: u32,
    pub drop_min_interval: u32,
}

impl RecordingRequest {
    pub fn parse(body: &[u8]) -> Result<Self, &'static str> {
        let raw: RawRequest = serde_json::from_slice(body).map_err(|_| "one strict recording JSON object required")?;
        let session = decimal(&raw.expected_session_id).ok_or("expected_session_id must be a decimal uint64 string")?;
        if raw.action == "stop" {
            // Presence matters: explicit null is not an omitted start-only field.
            if raw.format.is_some() || raw.storage.is_some() || raw.frame_skip.is_some() || raw.drop_min_interval.is_some() {
                return Err("stop accepts only action and expected_session_id");
            }
            return Ok(Self { start: false, expected_session_id: session, mp4: false, tf: false, frame_skip: 0, drop_min_interval: 0 });
        }
        if raw.action != "start" { return Err("action must be start or stop"); }
        let format = raw.format.as_ref().and_then(Value::as_str).ok_or("format required")?;
        let storage = raw.storage.as_ref().and_then(Value::as_str).ok_or("storage required")?;
        let number = |v: Option<Value>| v.and_then(|v| v.as_u64()).and_then(|v| u32::try_from(v).ok());
        let frame_skip = number(raw.frame_skip).ok_or("frame_skip must be uint32")?;
        let drop_min_interval = number(raw.drop_min_interval).ok_or("drop_min_interval must be uint32")?;
        if !matches!(format, "rosbag" | "mp4") || !matches!(storage, "runtime" | "tf") || frame_skip > 1 || drop_min_interval < 2 || (frame_skip == 1 && (format == "mp4" || drop_min_interval != 2)) {
            return Err("invalid recording format, storage, skip or interval");
        }
        Ok(Self { start: true, expected_session_id: session, mp4: format == "mp4", tf: storage == "tf", frame_skip, drop_min_interval })
    }
}
