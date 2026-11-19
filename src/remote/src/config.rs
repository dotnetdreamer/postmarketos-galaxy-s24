//! ~/.config/s24-remote.conf: `key = value` lines, `#` comments. Every key is optional.
//!
//!   port = 7180              where the tunnel reaches this program (127.0.0.1 only)
//!   tunnel = quick           quick: a new trycloudflare.com address each time, no account
//!                            token: a named Cloudflare tunnel (tunnel_token + hostname below)
//!                            none: no tunnel (this phone only, for tests)
//!   tunnel_token = …         a named tunnel's token (cloudflared tunnel token NAME)
//!   hostname = desk.example.org   that tunnel's public name, to show and to accept
//!   grace_minutes = 5        how long the desktop waits for a browser that went away
//!   phone_screen = off       off: the phone's own screen is off while a browser is connected
//!                            on: left as it is
//!   helper = /path           openandroiddex-pmos (default: the first one found, see HELPERS)
//!   cloudflared = /path      default: /usr/local/bin/cloudflared, then $PATH

use std::path::{Path, PathBuf};

pub const HELPERS: &[&str] = &[
    "/usr/local/libexec/s24-remote/openandroiddex-pmos",
    "~/.local/lib/openandroiddex/openandroiddex-pmos",
];

#[derive(Clone, Debug, PartialEq)]
pub enum Tunnel {
    Quick,
    Token { token: String },
    None,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub port: u16,
    pub tunnel: Tunnel,
    pub hostname: Option<String>,
    pub grace_secs: u64,
    pub phone_screen_off: bool,
    pub helper: PathBuf,
    pub cloudflared: PathBuf,
}

fn expand(p: &str) -> PathBuf {
    match p.strip_prefix("~/") {
        Some(rest) => crate::util::home().join(rest),
        None => PathBuf::from(p),
    }
}

fn find_in_path(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(name)).find(|p| p.is_file())
}

impl Config {
    pub fn load() -> Config {
        let text = std::fs::read_to_string(crate::util::config_file()).unwrap_or_default();
        Config::parse(&text)
    }

    pub fn parse(text: &str) -> Config {
        let mut c = Config {
            port: 7180,
            tunnel: Tunnel::Quick,
            hostname: None,
            grace_secs: 5 * 60,
            phone_screen_off: true,
            helper: HELPERS
                .iter()
                .map(|p| expand(p))
                .find(|p| p.is_file())
                .unwrap_or_else(|| expand(HELPERS[0])),
            cloudflared: [PathBuf::from("/usr/local/bin/cloudflared")]
                .into_iter()
                .find(|p| p.is_file())
                .or_else(|| find_in_path("cloudflared"))
                .unwrap_or_else(|| PathBuf::from("/usr/local/bin/cloudflared")),
        };
        let mut tunnel = "quick".to_string();
        let mut token = None;
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else { continue };
            let (k, v) = (k.trim(), v.trim());
            match k {
                "port" => c.port = v.parse().unwrap_or(c.port),
                "tunnel" => tunnel = v.to_lowercase(),
                "tunnel_token" => token = Some(v.to_string()),
                "hostname" if !v.is_empty() => c.hostname = Some(v.to_lowercase()),
                "grace_minutes" => c.grace_secs = v.parse::<u64>().map(|m| m * 60).unwrap_or(c.grace_secs),
                "phone_screen" => c.phone_screen_off = v != "on",
                "helper" => c.helper = expand(v),
                "cloudflared" => c.cloudflared = expand(v),
                _ => {}
            }
        }
        c.tunnel = match (tunnel.as_str(), token) {
            ("token", Some(t)) if !t.is_empty() => Tunnel::Token { token: t },
            ("none", _) => Tunnel::None,
            _ => Tunnel::Quick,
        };
        c
    }

    pub fn helper_ok(&self) -> bool {
        Path::new(&self.helper).is_file()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        let c = Config::parse("");
        assert_eq!(c.port, 7180);
        assert_eq!(c.tunnel, Tunnel::Quick);
        assert_eq!(c.grace_secs, 300);
        assert!(c.phone_screen_off);
    }

    #[test]
    fn keys() {
        let c = Config::parse(
            "# comment\nport = 9000\ntunnel = token\ntunnel_token = abc\nhostname = Desk.Example.org\n\
             grace_minutes = 2\nphone_screen = on\nnonsense\n",
        );
        assert_eq!(c.port, 9000);
        assert_eq!(c.tunnel, Tunnel::Token { token: "abc".into() });
        assert_eq!(c.hostname.as_deref(), Some("desk.example.org"));
        assert_eq!(c.grace_secs, 120);
        assert!(!c.phone_screen_off);
    }

    #[test]
    fn token_tunnel_without_token_is_quick() {
        assert_eq!(Config::parse("tunnel = token\n").tunnel, Tunnel::Quick);
        assert_eq!(Config::parse("tunnel = none\n").tunnel, Tunnel::None);
    }
}
