//! The helper: Open Android DeX's postmarketOS program (openandroiddex-pmos), which asks KWin for a
//! virtual screen, encodes it as H.264 and puts our pointer and keys on it.
//!
//! It speaks on stdin and stdout in frames of [kind u8][length u32 BE][payload]: kind 0 is JSON
//! text, kind 1 binary. Out of it: {"t":"ready"}, {"t":"state"}, {"t":"stats"}, {"t":"clip"} and
//! video, [0x01][key u8][pts u64 BE][Annex B]. Into it: {"t":"hello","acks":true,"w","h"},
//! {"t":"hb"} (it ends after 15 s without a word), {"t":"bye"}, {"t":"stop"}, {"t":"resize"},
//! {"t":"bitrate"}, {"t":"keyframe"}, {"t":"clip"}, {"t":"clipshare"}, and binary pointer
//! [0x10 mask x16 y16], key [0x11 down keysym32], ack [0x12]: it encodes at most 4 frames ahead of
//! the acks, which is how we pace it (core.rs).
//!
//! It ends when its stdin closes, so it never outlives us; PR_SET_PDEATHSIG makes sure.

use std::process::Stdio;

use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, Command};
use tokio::sync::mpsc;

use crate::core::Ev;
use crate::say;

pub const MSG_VIDEO: u8 = 0x01;
pub const IN_POINTER: u8 = 0x10;
pub const IN_KEY: u8 = 0x11;
pub const IN_ACK: u8 = 0x12;
/// From the page only, never to the helper: a byte that keeps the phone's Wi-Fi awake (app.js).
pub const IN_AWAKE: u8 = 0x13;

pub fn frame(kind: u8, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(5 + payload.len());
    v.push(kind);
    v.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    v.extend_from_slice(payload);
    v
}

pub struct Helper {
    tx: mpsc::UnboundedSender<Vec<u8>>,
    child: Child,
}

/// Children die with us, however we go.
pub fn die_with_parent(cmd: &mut Command) {
    unsafe {
        cmd.pre_exec(|| {
            libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
            Ok(())
        });
    }
}

impl Helper {
    pub fn spawn(
        path: &std::path::Path,
        args: &[String],
        gen: u64,
        events: mpsc::UnboundedSender<Ev>,
    ) -> std::io::Result<Helper> {
        let mut cmd = Command::new(path);
        cmd.arg("serve")
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        die_with_parent(&mut cmd);
        let mut child = cmd.spawn()?;
        let mut stdout = child.stdout.take().expect("stdout");
        let stderr = child.stderr.take().expect("stderr");
        let mut stdin = child.stdin.take().expect("stdin");

        // Out of it: frames, until it closes its stdout.
        let ev = events.clone();
        tokio::spawn(async move {
            let mut head = [0u8; 5];
            loop {
                if stdout.read_exact(&mut head).await.is_err() {
                    break;
                }
                let len = u32::from_be_bytes([head[1], head[2], head[3], head[4]]) as usize;
                if len > 32 << 20 {
                    say!("helper: a frame of {len} bytes, giving up on it");
                    break;
                }
                let mut data = vec![0u8; len];
                if stdout.read_exact(&mut data).await.is_err() {
                    break;
                }
                if ev.send(Ev::Helper { gen, kind: head[0], data }).is_err() {
                    break;
                }
            }
            let _ = ev.send(Ev::HelperOut { gen });
        });

        // Its log lines into ours.
        tokio::spawn(async move {
            let mut lines = BufReader::new(stderr).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                say!("helper: {line}");
            }
        });

        // Into it, in order.
        let (tx, mut rx) = mpsc::unbounded_channel::<Vec<u8>>();
        tokio::spawn(async move {
            while let Some(buf) = rx.recv().await {
                if stdin.write_all(&buf).await.is_err() {
                    break;
                }
            }
            // Dropping stdin closes it: the helper takes that as the end.
        });

        Ok(Helper { tx, child })
    }

    pub fn text(&self, json: &str) {
        let _ = self.tx.send(frame(0, json.as_bytes()));
    }

    pub fn binary(&self, data: &[u8]) {
        let _ = self.tx.send(frame(1, data));
    }

    pub fn ack(&self) {
        self.binary(&[IN_ACK]);
    }

    pub fn id(&self) -> Option<u32> {
        self.child.id()
    }

    /// {"t":"stop"}, then up to `wait` for it to put the screen away, then kill.
    pub async fn stop(mut self, wait: std::time::Duration) {
        self.text(r#"{"t":"stop"}"#);
        match tokio::time::timeout(wait, self.child.wait()).await {
            Ok(Ok(st)) => say!("helper: ended ({st})"),
            _ => {
                say!("helper: didn't end in {wait:?}, killing it");
                let _ = self.child.kill().await;
            }
        }
    }

    /// Whether it has ended, and how.
    pub fn try_status(&mut self) -> Option<String> {
        match self.child.try_wait() {
            Ok(Some(st)) => Some(st.to_string()),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames() {
        assert_eq!(frame(0, b"{}"), vec![0, 0, 0, 0, 2, b'{', b'}']);
        assert_eq!(frame(1, &[IN_ACK]), vec![1, 0, 0, 0, 1, 0x12]);
    }
}
