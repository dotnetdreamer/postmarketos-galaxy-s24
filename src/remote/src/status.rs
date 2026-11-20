//! What Remote Desktop is doing, for the quick setting, `s24-remote status` and the notification.

use crate::auth::spaced;
use crate::util::json_str;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum State {
    Starting,
    Ready,
    Connected,
    Error,
}

impl std::fmt::Display for State {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.write_str(match self {
            State::Starting => "starting",
            State::Ready => "ready",
            State::Connected => "connected",
            State::Error => "error",
        })
    }
}

#[derive(Clone, Debug)]
pub struct Status {
    pub state: State,
    /// The address browsers use, once there is one.
    pub url: Option<String>,
    /// An address being tried, not handed out yet.
    pub trying: Option<String>,
    /// The Wi-Fi door's address (lan.rs), when it is open.
    pub lan_url: Option<String>,
    pub code: String,
    /// Where the browser that is connected came from.
    pub viewer: Option<String>,
    /// Why, when state is Error; otherwise a note (or empty).
    pub message: String,
    /// No sign-ins for this many more seconds (too many wrong codes).
    pub locked_secs: u64,
}

impl Status {
    pub fn new(code: &str) -> Status {
        Status {
            state: State::Starting,
            url: None,
            trying: None,
            lan_url: None,
            code: code.to_string(),
            viewer: None,
            message: String::new(),
            locked_secs: 0,
        }
    }

    /// Whether `host` is ours: the address handed out, or one the tunnel is trying.
    pub fn is_our_host(&self, host: &str) -> bool {
        [self.url.as_deref(), self.trying.as_deref()].into_iter().flatten().any(|u| strip(u) == host)
    }

    pub fn summary(&self) -> String {
        match self.state {
            State::Starting => "Starting…".into(),
            State::Error => self.message.clone(),
            State::Connected => "A browser is connected".into(),
            State::Ready if self.locked_secs > 0 => "Too many wrong codes: wait, or turn off and on".into(),
            State::Ready => format!("Code {}", spaced(&self.code)),
        }
    }

    pub fn to_json(&self) -> String {
        format!(
            "{{\"state\":{},\"url\":{},\"lan\":{},\"code\":{},\"viewer\":{},\"message\":{},\"summary\":{}}}",
            json_str(&self.state.to_string()),
            self.url.as_deref().map(json_str).unwrap_or_else(|| "null".into()),
            self.lan_url.as_deref().map(json_str).unwrap_or_else(|| "null".into()),
            json_str(&self.code),
            self.viewer.as_deref().map(json_str).unwrap_or_else(|| "null".into()),
            json_str(&self.message),
            json_str(&self.summary())
        )
    }
}

fn strip(u: &str) -> &str {
    u.trim_start_matches("https://").trim_start_matches("http://")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn json_is_json() {
        let mut s = Status::new("12345678");
        s.url = Some("https://a-b-c-d.trycloudflare.com".into());
        s.state = State::Ready;
        let v: serde_json::Value = serde_json::from_str(&s.to_json()).unwrap();
        assert_eq!(v["summary"], "Code 1234 5678");
        assert_eq!(v["url"], "https://a-b-c-d.trycloudflare.com");
        assert!(v["lan"].is_null());
        s.lan_url = Some("https://10.0.0.30:7443".into());
        let v: serde_json::Value = serde_json::from_str(&s.to_json()).unwrap();
        assert_eq!(v["lan"], "https://10.0.0.30:7443");
        s.trying = Some("https://e-f-g-h.trycloudflare.com".into());
        assert!(s.is_our_host("a-b-c-d.trycloudflare.com") && s.is_our_host("e-f-g-h.trycloudflare.com"));
        assert!(!s.is_our_host("evil.example"));
    }
}
