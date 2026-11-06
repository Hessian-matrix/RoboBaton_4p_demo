use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    for path in ["VERSION", "scripts/build_sensor_web.sh", "scripts/verify_runtime_package.py", "web"] {
        println!("cargo::rerun-if-changed={}", root.join(path).display());
    }
    let compiler = Command::new(env::var_os("RUSTC").unwrap()).arg("--version").output().expect("Rust compiler identity");
    assert!(compiler.status.success(), "Rust compiler identity unavailable");
    let compiler = String::from_utf8(compiler.stdout).unwrap();
    let version = fs::read_to_string(root.join("VERSION")).expect("application VERSION");
    assert_eq!(compiler.trim(), "rustc 1.95.0 (59807616e 2026-04-14)", "sensor_web requires exactly Rust 1.95.0");
    let output = Command::new("python3").arg(root.join("scripts/verify_runtime_package.py")).arg("--web-build-info").output().expect("Web build identity verifier");
    assert!(output.status.success(), "Web build identity failed: {}", String::from_utf8_lossy(&output.stderr));
    let directory = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    fs::write(directory.join("sensor-web-build.json"), &output.stdout).unwrap();
    let identity = String::from_utf8(output.stdout).expect("UTF-8 build identity");
    let digest = Command::new("python3").arg(root.join("scripts/verify_runtime_package.py")).arg("--web-source-digest").output().expect("Web source digest");
    assert!(digest.status.success(), "Web source digest failed");
    let digest = String::from_utf8(digest.stdout).unwrap();
    let mut source = format!(r#"#[used]
#[unsafe(link_section = ".sensor_web")]
static BUILD_IDENTITY: [u8; {length}] = *include_bytes!(concat!(env!("OUT_DIR"), "/sensor-web-build.json"));
#[used]
#[unsafe(link_section = ".sensor_web_runtime")]
static RUNTIME_BUILD_IDENTITY: [u8; {length}] = *include_bytes!(concat!(env!("OUT_DIR"), "/sensor-web-build.json"));

const fn runtime_identity_text(start: usize, length: usize) -> &'static str {{
    match std::str::from_utf8(RUNTIME_BUILD_IDENTITY.split_at(start).1.split_at(length).0) {{
        Ok(value) => value,
        Err(_) => panic!("invalid runtime build identity text"),
    }}
}}
"#, length = identity.len());
    for (name, prefix, value) in [
        ("VERSION", "\"version\":\"", version.trim()),
        ("SOURCE_DIGEST", "\"source_digest\":\"", digest.trim()),
        ("COMPILER", "\"compiler\":\"", compiler.trim()),
        ("WEBRTC_VERSION", "\"str0m@", "0.24.1"),
    ] {
        let start = identity.find(&format!("{prefix}{value}\"")).expect("runtime identity field") + prefix.len();
        source.push_str(&format!("static {name}: &str = runtime_identity_text({start}, {});\n", value.len()));
    }
    fs::write(directory.join("identity.rs"), source).unwrap();
}
