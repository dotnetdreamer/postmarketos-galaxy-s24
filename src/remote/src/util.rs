//! Small things several parts need: randomness, encodings, paths, the log.

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::PathBuf;
use std::sync::Mutex;

/// Bytes from the kernel's random source. Panics only if /dev/urandom can't be read, which on Linux
/// means something far worse is wrong.
pub fn random_bytes(n: usize) -> Vec<u8> {
    let mut buf = vec![0u8; n];
    File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut buf))
        .expect("/dev/urandom");
    buf
}

/// A uniformly random number below `below` (rejection sampling, so no digit is likelier than another).
pub fn random_below(below: u32) -> u32 {
    let limit = u32::MAX - u32::MAX % below;
    loop {
        let b = random_bytes(4);
        let v = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
        if v < limit {
            return v % below;
        }
    }
}

pub fn hex(bytes: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        s.push(H[(b >> 4) as usize] as char);
        s.push(H[(b & 15) as usize] as char);
    }
    s
}

pub fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        out.push(T[(n >> 18) as usize & 63] as char);
        out.push(T[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    out
}

/// Equal without telling, through timing, how much of it was.
pub fn same_secret(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut d = 0u8;
    for (x, y) in a.iter().zip(b) {
        d |= x ^ y;
    }
    d == 0
}

/// $XDG_RUNTIME_DIR/s24-remote: the running copy's pid and state (gone with the session).
pub fn runtime_dir() -> PathBuf {
    let base = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })));
    base.join("s24-remote")
}

/// ~/.cache/s24-remote: the log, and what has to be put back after a run that didn't end.
pub fn cache_dir() -> PathBuf {
    let base = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".cache"));
    base.join("s24-remote")
}

pub fn config_file() -> PathBuf {
    if let Some(p) = std::env::var_os("S24_REMOTE_CONFIG") {
        return PathBuf::from(p);
    }
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".config"));
    base.join("s24-remote.conf")
}

pub fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/tmp"))
}

/// Written whole or not at all: a reader never sees half a file.
pub fn write_atomic(path: &PathBuf, text: &str) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, text)?;
    fs::rename(&tmp, path)
}

/// JSON's string syntax, quotes included.
pub fn json_str(s: &str) -> String {
    serde_json::Value::String(s.to_owned()).to_string()
}

// ── the log ──────────────────────────────────────────────────────────────

static LOG: Mutex<Option<File>> = Mutex::new(None);

pub fn log_open() {
    let dir = cache_dir();
    let _ = fs::create_dir_all(&dir);
    let path = dir.join("s24-remote.log");
    if fs::metadata(&path).map(|m| m.len() > 512 * 1024).unwrap_or(false) {
        let _ = fs::rename(&path, dir.join("s24-remote.log.old"));
    }
    if let Ok(f) = OpenOptions::new().create(true).append(true).open(&path) {
        *LOG.lock().unwrap() = Some(f);
    }
}

pub fn stamp() -> String {
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe {
        let t = libc::time(std::ptr::null_mut());
        libc::localtime_r(&t, &mut tm);
    }
    format!("{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

pub fn log_line(msg: &str) {
    let line = format!("{} {}\n", stamp(), msg);
    let mut guard = LOG.lock().unwrap();
    match guard.as_mut() {
        Some(f) => {
            let _ = f.write_all(line.as_bytes());
        }
        None => eprint!("{line}"),
    }
}

#[macro_export]
macro_rules! say {
    ($($arg:tt)*) => { $crate::util::log_line(&format!($($arg)*)) };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_matches_the_standard() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64(&[0xff, 0xfe, 0x00]), "//4A");
    }

    #[test]
    fn random_below_stays_below() {
        for _ in 0..1000 {
            assert!(random_below(10) < 10);
        }
    }

    #[test]
    fn secrets_compare() {
        assert!(same_secret(b"abc", b"abc"));
        assert!(!same_secret(b"abc", b"abd"));
        assert!(!same_secret(b"abc", b"ab"));
    }
}
