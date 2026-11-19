//! The way in from the internet: Cloudflare's tunnel program, cloudflared.
//!
//! The phone has no address the internet can reach (mobile data is behind the carrier's NAT, Wi-Fi
//! behind a router), so cloudflared connects out to Cloudflare and browsers reach the phone through
//! it: https on port 443 with a real certificate, the one way into the internet that schools and
//! offices leave open. WebSockets go through it.
//!
//! - `quick` (the default): `cloudflared tunnel --url`, no account. Cloudflare makes up an address,
//!   https://<four words>.trycloudflare.com, new each time cloudflared starts
//! - `token`: a named tunnel of the owner's own Cloudflare account, with a fixed address
//!
//! cloudflared is restarted if it stops, after 2, 4, 8 … 60 s. Before the address is handed out it
//! is tried from the phone itself (through Cloudflare and back): a new quick address takes a few
//! seconds to be known everywhere.

use std::process::Stdio;
use std::time::Duration;

use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;
use tokio::sync::watch;

use crate::config::{Config, Tunnel};
use crate::say;

#[derive(Clone, Debug, PartialEq)]
pub enum Way {
    Starting,
    /// The address is known but not yet tried: requests to it must be let in for the try.
    Found(String),
    /// The address browsers use.
    Ready(String),
    /// Not reachable right now, and why.
    Down(String),
}

/// The quick tunnel's address in one of cloudflared's log lines.
pub fn quick_address(line: &str) -> Option<String> {
    let i = line.find("https://")?;
    let rest = &line[i..];
    let end = rest.find(|c: char| !(c.is_ascii_alphanumeric() || ":/.-".contains(c))).unwrap_or(rest.len());
    let url = &rest[..end];
    let host = url.trim_start_matches("https://");
    (host.ends_with(".trycloudflare.com") && host.len() > ".trycloudflare.com".len() && !host.starts_with("api."))
        .then(|| url.to_string())
}

/// Tried from here: does a browser get to us through this address?
///
/// The name is looked up over DNS-over-HTTPS, at Cloudflare itself. A new quick address isn't
/// known for its first few seconds, and asking the phone's usual DNS server (often the home
/// router, which the computer's browser asks too) that early can leave "no such name" in its cache
/// for minutes. If this curl can't do DoH, the ordinary lookup it is.
async fn reachable(url: &str) -> bool {
    let target = format!("{url}/api/ping");
    let base = ["-s", "-o", "/dev/null", "-w", "%{http_code}", "--max-time", "6"];
    let out = Command::new("curl")
        .args(base)
        .args(["--doh-url", "https://cloudflare-dns.com/dns-query"])
        .arg(&target)
        .stdin(Stdio::null())
        .output()
        .await;
    match out {
        Ok(o) if o.stdout == b"200" => true,
        // 2: unknown option, 4: a feature this curl was built without
        Ok(o) if matches!(o.status.code(), Some(2) | Some(4)) => {
            let plain = Command::new("curl").args(base).arg(&target).stdin(Stdio::null()).output().await;
            matches!(plain, Ok(p) if p.stdout == b"200")
        }
        _ => false,
    }
}

pub async fn run(cfg: Config, way: watch::Sender<Way>) {
    if cfg.tunnel == Tunnel::None {
        let _ = way.send(Way::Ready(format!("http://127.0.0.1:{}", cfg.port)));
        return;
    }
    if !cfg.cloudflared.is_file() {
        let _ = way.send(Way::Down(format!(
            "cloudflared isn't installed ({}): see docs/remote-desktop.md",
            cfg.cloudflared.display()
        )));
        return;
    }
    let mut backoff = 2u64;
    loop {
        let _ = way.send(Way::Starting);
        let mut cmd = Command::new(&cfg.cloudflared);
        cmd.arg("tunnel").arg("--no-autoupdate");
        match &cfg.tunnel {
            Tunnel::Token { token } => {
                cmd.args(["run", "--token", token]);
            }
            _ => {
                cmd.args(["--url", &format!("http://127.0.0.1:{}", cfg.port)]);
            }
        }
        cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped()).kill_on_drop(true);
        crate::helper::die_with_parent(&mut cmd);
        let started = std::time::Instant::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                let _ = way.send(Way::Down(format!("cloudflared didn't start: {e}")));
                tokio::time::sleep(Duration::from_secs(backoff)).await;
                backoff = (backoff * 2).min(60);
                continue;
            }
        };
        say!("tunnel: cloudflared started (pid {})", child.id().unwrap_or(0));
        let mut lines = BufReader::new(child.stderr.take().unwrap()).lines();
        let mut url: Option<String> = match &cfg.tunnel {
            Tunnel::Token { .. } => cfg.hostname.as_ref().map(|h| format!("https://{h}")),
            _ => None,
        };
        if let Some(u) = &url {
            let _ = way.send(Way::Found(u.clone()));
        }
        let mut connected = 0u32;
        let mut announced = false;
        let mut checking: Option<tokio::task::JoinHandle<bool>> = None;
        loop {
            tokio::select! {
                line = lines.next_line() => {
                    let Ok(Some(line)) = line else { break };
                    if url.is_none() {
                        if let Some(u) = quick_address(&line) {
                            say!("tunnel: address {u}");
                            let _ = way.send(Way::Found(u.clone()));
                            url = Some(u);
                        }
                    }
                    if line.contains("Registered tunnel connection") {
                        connected += 1;
                        if connected == 1 {
                            say!("tunnel: connected to Cloudflare");
                        }
                    } else if line.contains(" ERR ") || line.contains("level=error") {
                        say!("tunnel: {}", line.trim());
                    }
                    if !announced && checking.is_none() && connected > 0 {
                        if let Some(u) = url.clone() {
                            checking = Some(tokio::spawn(async move {
                                // A new address takes a few seconds to resolve everywhere.
                                for _ in 0..30 {
                                    if reachable(&u).await { return true; }
                                    tokio::time::sleep(Duration::from_secs(2)).await;
                                }
                                false
                            }));
                        }
                    }
                }
                done = async { checking.as_mut().unwrap().await }, if checking.is_some() => {
                    checking = None;
                    announced = true;
                    let ok = done.unwrap_or(false);
                    if !ok {
                        say!("tunnel: couldn't reach the address from the phone itself; handing it out anyway");
                    }
                    backoff = 2;
                    let _ = way.send(Way::Ready(url.clone().unwrap()));
                }
            }
        }
        let status = child.wait().await.map(|s| s.to_string()).unwrap_or_default();
        say!("tunnel: cloudflared ended ({status}) after {:?}", started.elapsed());
        if started.elapsed() > Duration::from_secs(120) {
            backoff = 2;
        }
        let _ = way.send(Way::Down("The internet connection to Cloudflare dropped; trying again".into()));
        tokio::time::sleep(Duration::from_secs(backoff)).await;
        backoff = (backoff * 2).min(60);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_the_quick_address() {
        let line = "2026-11-19T10:00:00Z INF |  https://seasonal-beside-tired-reasonably.trycloudflare.com                  |";
        assert_eq!(
            quick_address(line).as_deref(),
            Some("https://seasonal-beside-tired-reasonably.trycloudflare.com")
        );
        assert_eq!(quick_address("INF Requesting new quick Tunnel on trycloudflare.com..."), None);
        assert_eq!(quick_address("see https://developers.cloudflare.com/cloudflare-one/"), None);
        assert_eq!(quick_address("https://api.trycloudflare.com/tunnel"), None);
    }
}
