//! The desktop and the browser: the helper's screen, one browser at a time, and the pace between.
//!
//! One task owns all of it; the HTTP side, the helper's pipes and the power key talk to it through
//! `Ev`. A browser that comes takes over from one that was there. A browser that goes leaves the
//! screen up for `grace_minutes` (a reload or a dropped Wi-Fi comes back to the same windows), then
//! the phone is locked, its screen comes back and goes dark, and the screen is put away.

use std::collections::VecDeque;
use std::sync::Arc;
use std::time::{Duration, Instant};

use bytes::Bytes;
use serde_json::{json, Value};
use tokio::sync::{mpsc, oneshot, watch};

use crate::config::Config;
use crate::flow::{fit, Flow, Quality, Release};
use crate::h264;
use crate::helper::{Helper, IN_KEY, IN_POINTER, MSG_VIDEO};
use crate::phone::{self, Phone};
use crate::say;
use crate::status::{State, Status};
use crate::util::base64;

pub enum Ev {
    /// A signed-in browser said hello.
    Open { id: u64, tx: mpsc::UnboundedSender<Out>, hello: Value, who: String },
    Text { id: u64, v: Value },
    /// Pointer or key, as the helper takes them.
    Input { id: u64, data: Vec<u8> },
    Ack { id: u64, seq: u32 },
    Closed { id: u64 },
    Helper { gen: u64, kind: u8, data: Vec<u8> },
    HelperOut { gen: u64 },
    PowerKey,
    /// Remote Desktop is being turned off (by someone at the phone).
    Stop(oneshot::Sender<()>),
}

/// To a browser.
pub enum Out {
    Text(String),
    Bin(Bytes),
    Close,
}

struct Viewer {
    id: u64,
    tx: mpsc::UnboundedSender<Out>,
    who: String,
    /// Frames are dropped until a key frame: it's new, or it missed some.
    need_key: bool,
    /// The decoder's description (SPS/PPS) went to it.
    config_sent: bool,
    paused: bool,
    seq: u32,
    t0: Instant,
    locked: Option<bool>,
}

impl Viewer {
    fn text(&self, v: Value) {
        let _ = self.tx.send(Out::Text(v.to_string()));
    }
}

pub struct Core {
    cfg: Config,
    bus: zbus::Connection,
    events: mpsc::UnboundedSender<Ev>,
    status: Arc<watch::Sender<Status>>,
    gen: u64,
    helper: Option<Helper>,
    ready: bool,
    starts: VecDeque<Instant>,
    viewer: Option<Viewer>,
    css: (u32, u32),
    asked: (u32, u32),
    flow: Flow,
    sps: Vec<u8>,
    pps: Vec<u8>,
    video_config: Option<String>,
    grace_until: Option<Instant>,
    phone: Phone,
    /// The browser wants the phone's own screen off (and it isn't back on by the power key).
    phone_off: bool,
    pace_at: Option<Instant>,
    last_hb: Instant,
    last_key_ask: Option<Instant>,
    last_second: Instant,
    frames: u32,
    bytes: u64,
    encoder: String,
    last_lock_look: Instant,
    last_sync: Instant,
}

const HELPER_NAME: &str = "S24Remote";

impl Core {
    pub fn new(cfg: Config, bus: zbus::Connection, events: mpsc::UnboundedSender<Ev>, status: Arc<watch::Sender<Status>>) -> Core {
        let now = Instant::now();
        Core {
            phone: Phone::new(bus.clone(), events.clone()),
            phone_off: cfg.phone_screen_off,
            cfg,
            bus,
            events,
            status,
            gen: 0,
            helper: None,
            ready: false,
            starts: VecDeque::new(),
            viewer: None,
            css: (1280, 720),
            asked: (0, 0),
            flow: Flow::new(Quality::Medium, now),
            sps: vec![],
            pps: vec![],
            video_config: None,
            grace_until: None,
            pace_at: None,
            last_hb: now,
            last_key_ask: None,
            last_second: now,
            frames: 0,
            bytes: 0,
            encoder: String::new(),
            last_lock_look: now,
            last_sync: now,
        }
    }

    pub async fn run(mut self, mut rx: mpsc::UnboundedReceiver<Ev>) {
        let mut tick = tokio::time::interval(Duration::from_millis(100));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            let pace = self.pace_at;
            tokio::select! {
                ev = rx.recv() => match ev {
                    Some(Ev::Stop(done)) => {
                        self.end(false).await;
                        let _ = done.send(());
                        return;
                    }
                    Some(ev) => self.handle(ev).await,
                    None => {
                        self.end(false).await;
                        return;
                    }
                },
                _ = tick.tick() => self.tick().await,
                _ = async { tokio::time::sleep_until(pace.unwrap().into()).await }, if pace.is_some() => {
                    self.pace_at = None;
                    self.pump(Instant::now());
                }
            }
        }
    }

    fn is_current(&self, id: u64) -> bool {
        self.viewer.as_ref().is_some_and(|v| v.id == id)
    }

    fn set_status(&self, f: impl FnOnce(&mut Status)) {
        self.status.send_modify(f);
    }

    async fn handle(&mut self, ev: Ev) {
        let now = Instant::now();
        match ev {
            Ev::Open { id, tx, hello, who } => self.open(id, tx, hello, who, now).await,
            Ev::Text { id, v } if self.is_current(id) => self.text(v, now).await,
            Ev::Input { id, data } if self.is_current(id) => {
                let ok = matches!((data.first(), data.len()), (Some(&IN_POINTER), 6) | (Some(&IN_KEY), 6));
                if let (true, Some(h)) = (ok, &self.helper) {
                    h.binary(&data);
                }
            }
            Ev::Ack { id, seq } if self.is_current(id) => {
                self.flow.acked(seq, now);
                self.pump(now);
            }
            Ev::Closed { id } if self.is_current(id) => {
                let v = self.viewer.take().unwrap();
                say!("browser {} went; the screen waits {} s for it", v.who, self.cfg.grace_secs);
                if let Some(h) = &self.helper {
                    h.text(r#"{"t":"bye"}"#);
                }
                self.flow.helper_reset();
                self.flow.browser_reset();
                self.grace_until = Some(now + Duration::from_secs(self.cfg.grace_secs));
                self.set_status(|s| {
                    s.viewer = None;
                    if s.state == State::Connected {
                        s.state = State::Ready;
                    }
                });
            }
            Ev::Helper { gen, kind, data } if gen == self.gen => self.from_helper(kind, data, now).await,
            Ev::HelperOut { gen } if gen == self.gen => self.helper_gone(now).await,
            Ev::PowerKey => {
                if self.phone.is_off() {
                    say!("power key: the phone's screen back, locked");
                    phone::lock(&self.bus).await;
                    self.phone.screen_on(false, false).await;
                    self.phone_off = false;
                    // PowerDevil turned the screens off for the same press: on again, to the lock screen.
                    tokio::time::sleep(Duration::from_millis(700)).await;
                    let _ = phone::kscreen(&["--dpms", "on"]).await;
                    if let Some(v) = &self.viewer {
                        v.text(json!({"t": "phonescreen", "on": true, "why": "power"}));
                    }
                } else if self.viewer.is_some() && self.ready {
                    // The phone's screen is on beside the browser's: the press turns off the phone's,
                    // not both, as PowerDevil's answer to it would.
                    say!("power key: the phone's screen off again; the browser's stays");
                    self.phone_off = true;
                    if let Err(e) = self.phone.screen_off().await {
                        say!("phone: {e}");
                    }
                    tokio::time::sleep(Duration::from_millis(700)).await;
                    let _ = phone::kscreen(&["--dpms", "on"]).await;
                    if let Some(v) = &self.viewer {
                        v.text(json!({"t": "phonescreen", "on": !self.phone.is_off(), "why": "power"}));
                    }
                }
            }
            _ => {}
        }
    }

    async fn open(&mut self, id: u64, tx: mpsc::UnboundedSender<Out>, hello: Value, who: String, now: Instant) {
        if let Some(old) = self.viewer.take() {
            say!("browser {} replaced by {who}", old.who);
            let _ = old.tx.send(Out::Text(json!({"t": "bye", "reason": "taken"}).to_string()));
            let _ = old.tx.send(Out::Close);
            if let Some(h) = &self.helper {
                h.text(r#"{"t":"bye"}"#);
            }
        }
        let q = hello["quality"].as_str().and_then(Quality::parse).unwrap_or(Quality::Medium);
        self.css = (
            hello["w"].as_u64().unwrap_or(1280).clamp(320, 8192) as u32,
            hello["h"].as_u64().unwrap_or(720).clamp(240, 8192) as u32,
        );
        if let Some(on) = hello["phoneScreen"].as_bool() {
            self.phone_off = !on;
        } else {
            self.phone_off = self.cfg.phone_screen_off;
        }
        self.flow = Flow::new(q, now);
        self.grace_until = None;
        let v = Viewer {
            id,
            tx,
            who: who.clone(),
            need_key: true,
            config_sent: false,
            paused: false,
            seq: 0,
            t0: now,
            locked: None,
        };
        v.text(json!({"t": "welcome", "quality": q.name(), "phoneScreen": !self.phone_off}));
        self.viewer = Some(v);
        say!("browser {who} connected ({}x{} window, {})", self.css.0, self.css.1, q.name());
        self.set_status(|s| {
            s.state = State::Connected;
            s.viewer = Some(who);
        });
        let size = fit(self.css.0, self.css.1, q.cap());
        if self.helper.is_none() {
            self.start_helper(size, now);
        }
        self.attach(size);
        if self.ready {
            if let Some(v) = &self.viewer {
                v.text(json!({"t": "state", "state": "running"}));
            }
            self.phone_screen().await;
        } else if let Some(v) = &self.viewer {
            v.text(json!({"t": "state", "state": "starting"}));
        }
    }

    /// The browser on the helper's screen: hello (it counts frames from nothing, sends a key frame).
    fn attach(&mut self, size: (u32, u32)) {
        if let Some(h) = &self.helper {
            h.text(&json!({"t": "hello", "v": 1, "acks": true, "w": size.0, "h": size.1, "bitrate": self.flow.bitrate}).to_string());
            self.asked = size;
            self.flow.helper_reset();
        }
    }

    fn start_helper(&mut self, size: (u32, u32), now: Instant) {
        while self.starts.front().is_some_and(|t| now.duration_since(*t) > Duration::from_secs(120)) {
            self.starts.pop_front();
        }
        if self.starts.len() >= 3 {
            say!("helper: started 3 times in 2 minutes, not again");
            if let Some(v) = &self.viewer {
                v.text(json!({"t": "state", "state": "failed", "message": "The phone's desktop keeps stopping. Turn Remote Desktop off and on, and try again."}));
            }
            return;
        }
        if !self.cfg.helper_ok() {
            say!("helper: not found at {}", self.cfg.helper.display());
            if let Some(v) = &self.viewer {
                v.text(json!({"t": "state", "state": "failed", "message": "The screen program (openandroiddex-pmos) isn't installed on the phone."}));
            }
            return;
        }
        self.starts.push_back(now);
        self.gen += 1;
        let args: Vec<String> = [
            "--width", &size.0.to_string(), "--height", &size.1.to_string(), "--scale", "1", "--cap", "2560",
            "--name", HELPER_NAME, "--bitrate", &self.flow.bitrate.to_string(),
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        match Helper::spawn(&self.cfg.helper, &args, self.gen, self.events.clone()) {
            Ok(h) => {
                say!("helper: started (pid {}) for a {}x{} screen", h.id().unwrap_or(0), size.0, size.1);
                self.helper = Some(h);
                self.ready = false;
                self.sps.clear();
                self.pps.clear();
                self.video_config = None;
                self.last_hb = now;
            }
            Err(e) => {
                say!("helper: couldn't start: {e}");
                if let Some(v) = &self.viewer {
                    v.text(json!({"t": "state", "state": "failed", "message": format!("The phone's desktop couldn't start: {e}")}));
                }
            }
        }
    }

    /// The phone's own screen as the browser wants it.
    async fn phone_screen(&mut self) {
        if !self.ready || self.viewer.is_none() {
            return;
        }
        if self.phone_off && !self.phone.is_off() {
            if let Err(e) = self.phone.screen_off().await {
                say!("phone: screen left on: {e}");
            }
        } else if !self.phone_off {
            self.phone.ensure_on().await;
        }
        if let Some(v) = &self.viewer {
            v.text(json!({"t": "phonescreen", "on": !self.phone.is_off()}));
        }
    }

    fn resize(&mut self) {
        let size = fit(self.css.0, self.css.1, self.flow.quality.cap());
        if size != self.asked {
            if let Some(h) = &self.helper {
                h.text(&json!({"t": "resize", "w": size.0, "h": size.1}).to_string());
                say!("screen: asked for {}x{}", size.0, size.1);
            }
            self.asked = size;
        }
    }

    fn ask_key(&mut self, now: Instant) {
        if self.last_key_ask.is_some_and(|t| now.duration_since(t) < Duration::from_millis(400)) {
            return;
        }
        self.last_key_ask = Some(now);
        if let Some(h) = &self.helper {
            h.text(r#"{"t":"keyframe"}"#);
        }
    }

    async fn text(&mut self, v: Value, now: Instant) {
        let t = v["t"].as_str().unwrap_or("");
        match t {
            "resize" => {
                if let (Some(w), Some(h)) = (v["w"].as_u64(), v["h"].as_u64()) {
                    self.css = (w.clamp(320, 8192) as u32, h.clamp(240, 8192) as u32);
                    self.resize();
                }
            }
            "quality" => {
                if let Some(q) = v["q"].as_str().and_then(Quality::parse) {
                    if q != self.flow.quality {
                        self.flow.set_quality(q, now);
                        say!("quality: {}", q.name());
                        self.resize();
                        if let Some(h) = &self.helper {
                            h.text(&json!({"t": "bitrate", "bitrate": self.flow.bitrate}).to_string());
                        }
                    }
                    if let Some(vw) = &self.viewer {
                        vw.text(json!({"t": "quality", "q": q.name()}));
                    }
                }
            }
            "keyframe" => {
                if let Some(vw) = self.viewer.as_mut() {
                    vw.need_key = true;
                }
                self.ask_key(now);
            }
            "ping" => {
                if let Some(vw) = &self.viewer {
                    vw.text(json!({"t": "pong", "n": v["n"]}));
                }
            }
            "clip" => {
                if let (Some(text), Some(h)) = (v["text"].as_str(), &self.helper) {
                    if text.len() <= 1 << 20 {
                        h.text(&json!({"t": "clip", "text": text}).to_string());
                    }
                }
            }
            "clipshare" => {
                if let Some(h) = &self.helper {
                    h.text(&json!({"t": "clipshare", "on": v["on"].as_bool().unwrap_or(false)}).to_string());
                }
            }
            "phonescreen" => {
                self.phone_off = !v["on"].as_bool().unwrap_or(false);
                say!("phone: the browser asks for its screen {}", if self.phone_off { "off" } else { "on" });
                self.phone_screen().await;
            }
            "pause" => {
                if let Some(vw) = self.viewer.as_mut() {
                    vw.paused = true;
                }
            }
            "resume" => {
                if let Some(vw) = self.viewer.as_mut() {
                    vw.paused = false;
                    vw.need_key = true;
                }
                self.flow.browser_reset();
                self.ask_key(now);
            }
            _ => {}
        }
    }

    async fn from_helper(&mut self, kind: u8, data: Vec<u8>, now: Instant) {
        if kind == 1 {
            if data.len() >= 10 && data[0] == MSG_VIDEO {
                self.video(&data, now);
            }
            return;
        }
        let Ok(v) = serde_json::from_slice::<Value>(&data) else { return };
        match v["t"].as_str().unwrap_or("") {
            "ready" => {
                self.ready = true;
                self.encoder = v["encoder"].as_str().unwrap_or("").to_string();
                say!("helper: ready, screen {} ({})", v["output"].as_str().unwrap_or("?"), self.encoder);
                if let Some(vw) = &self.viewer {
                    vw.text(json!({"t": "state", "state": "running"}));
                }
                self.phone_screen().await;
            }
            "state" => {
                let st = v["state"].as_str().unwrap_or("");
                if st == "failed" || st == "stopped" {
                    say!("helper: {st}: {}", v["message"].as_str().unwrap_or(""));
                }
                if let Some(vw) = &self.viewer {
                    vw.text(v);
                }
            }
            "clip" => {
                if let Some(vw) = &self.viewer {
                    vw.text(json!({"t": "clip", "text": v["text"]}));
                }
            }
            _ => {}
        }
    }

    fn video(&mut self, data: &[u8], now: Instant) {
        self.flow.frame_in();
        let f = h264::convert(&data[10..]);
        if let (Some(sps), Some(pps)) = (&f.sps, &f.pps) {
            if *sps != self.sps || *pps != self.pps {
                let (w, h) = h264::sps_size(sps).unwrap_or((0, 0));
                self.video_config = Some(
                    json!({"t": "video", "codec": h264::codec_string(sps), "desc": base64(&h264::avcc(sps, pps)), "w": w, "h": h})
                        .to_string(),
                );
                say!("video: {}x{} {}", w, h, h264::codec_string(sps));
                self.sps = sps.clone();
                self.pps = pps.clone();
                if let Some(v) = self.viewer.as_mut() {
                    v.config_sent = false;
                }
            }
        }
        let key = f.key || data[1] != 0;
        let stalled = self.flow.stalled(now);
        let mut want_key = false;
        if let Some(v) = self.viewer.as_mut() {
            if v.paused || stalled || self.flow.overfull() {
                v.need_key = true;
            } else if v.need_key && !key {
                want_key = true;
            } else if !f.avc.is_empty() {
                let mut ok = true;
                if !v.config_sent {
                    match &self.video_config {
                        Some(c) => {
                            let _ = v.tx.send(Out::Text(c.clone()));
                            v.config_sent = true;
                        }
                        None => ok = false,
                    }
                }
                if ok {
                    v.need_key = false;
                    v.seq = v.seq.wrapping_add(1);
                    let ts = now.duration_since(v.t0).as_millis() as u32;
                    let mut msg = Vec::with_capacity(10 + f.avc.len());
                    msg.push(MSG_VIDEO);
                    msg.push(key as u8);
                    msg.extend_from_slice(&v.seq.to_be_bytes());
                    msg.extend_from_slice(&ts.to_be_bytes());
                    msg.extend_from_slice(&f.avc);
                    let n = msg.len();
                    if v.tx.send(Out::Bin(Bytes::from(msg))).is_ok() {
                        self.flow.sent(v.seq, n, key, now);
                        self.frames += 1;
                        self.bytes += n as u64;
                    }
                }
            }
        }
        if want_key {
            self.ask_key(now);
        }
        self.pump(now);
    }

    /// Acknowledge frames to the helper as far as the pace allows.
    fn pump(&mut self, now: Instant) {
        let Some(h) = &self.helper else { return };
        let slow = match &self.viewer {
            None => true,
            Some(v) => v.paused || self.flow.stalled(now),
        };
        loop {
            match self.flow.release(now, slow) {
                Release::Now => h.ack(),
                Release::At(t) => {
                    self.pace_at = Some(t);
                    break;
                }
                Release::Wait => break,
            }
        }
    }

    async fn helper_gone(&mut self, now: Instant) {
        let status = match self.helper.as_mut() {
            Some(h) => {
                tokio::time::sleep(Duration::from_millis(50)).await;
                h.try_status().unwrap_or_else(|| "?".into())
            }
            None => return,
        };
        say!("helper: gone ({status})");
        self.helper = None;
        self.ready = false;
        self.flow.helper_reset();
        // Its screen is gone, and KWin has given the panel back: let go of the touch as well.
        if self.phone.is_off() {
            self.phone.screen_on(false, false).await;
        }
        self.phone.stop_watching();
        if self.viewer.is_some() {
            let size = fit(self.css.0, self.css.1, self.flow.quality.cap());
            if let Some(v) = self.viewer.as_mut() {
                v.need_key = true;
                v.config_sent = false;
            }
            self.flow.browser_reset();
            self.start_helper(size, now);
            if self.helper.is_some() {
                if let Some(v) = &self.viewer {
                    v.text(json!({"t": "state", "state": "starting", "message": "The phone's desktop stopped; starting it again"}));
                }
                self.attach(size);
            }
        }
    }

    async fn tick(&mut self) {
        let now = Instant::now();
        if let Some(h) = &self.helper {
            if now.duration_since(self.last_hb) >= Duration::from_secs(2) {
                h.text(r#"{"t":"hb"}"#);
                self.last_hb = now;
            }
        }
        if self.viewer.is_none() && self.grace_until.is_some_and(|t| now >= t) {
            say!("no browser for {} s: putting the screen away", self.cfg.grace_secs);
            self.grace_until = None;
            self.end(true).await;
            return;
        }
        if now.duration_since(self.last_second) >= Duration::from_secs(1) {
            let secs = now.duration_since(self.last_second).as_secs_f64();
            self.last_second = now;
            let adjusted = if self.viewer.is_some() { self.flow.adjust(now) } else { None };
            if let Some(b) = adjusted {
                say!("bitrate: {} kbit/s (round trip {} ms, link's own {} ms)", b / 1000, self.flow.srtt().as_millis(), self.flow.min_rtt().as_millis());
                if let Some(h) = &self.helper {
                    h.text(&json!({"t": "bitrate", "bitrate": b}).to_string());
                }
            }
            if let Some(v) = &self.viewer {
                v.text(json!({
                    "t": "stats",
                    "fps": (self.frames as f64 / secs).round(),
                    "kbps": (self.bytes as f64 * 8.0 / 1000.0 / secs).round(),
                    "rtt": self.flow.srtt().as_millis() as u64,
                    "bitrate": self.flow.bitrate / 1000,
                    "inflight": self.flow.in_flight(),
                    "w": self.asked.0, "h": self.asked.1,
                    "quality": self.flow.quality.name(),
                    "encoder": self.encoder,
                }));
            }
            self.frames = 0;
            self.bytes = 0;
        }
        // Stalled: no acknowledgement for 2 s. A frozen tab answers nothing, but a page whose decoder
        // was reset has lost the frames inside it, and their acknowledgements with them: asked to
        // sync, it acknowledges everything it has, and the frames flow again (from a key frame).
        if self.flow.stalled(now) && now.duration_since(self.last_sync) >= Duration::from_secs(1) {
            self.last_sync = now;
            if let Some(v) = &self.viewer {
                v.text(json!({"t": "sync", "seq": v.seq}));
            }
        }
        // The lock screen: the browser says so, since it's the phone's PIN that opens it.
        if self.viewer.is_some() && now.duration_since(self.last_lock_look) >= Duration::from_secs(2) {
            self.last_lock_look = now;
            let locked = phone::locked(&self.bus).await;
            if let Some(v) = self.viewer.as_mut() {
                if locked.is_some() && locked != v.locked {
                    v.locked = locked;
                    v.text(json!({"t": "locked", "on": locked.unwrap()}));
                }
            }
        }
        self.pump(now);
    }

    /// The screen put away. `left`: the browser went and didn't come back, so the phone is locked
    /// and dark as it was in the pocket; otherwise someone at the phone turned this off.
    async fn end(&mut self, left: bool) {
        if let Some(v) = self.viewer.take() {
            v.text(json!({"t": "bye", "reason": "off"}));
            let _ = v.tx.send(Out::Close);
        }
        let was_off = self.phone.is_off();
        if left && self.helper.is_some() {
            phone::lock(&self.bus).await;
        }
        if let Some(h) = self.helper.take() {
            h.stop(Duration::from_secs(6)).await;
        }
        self.ready = false;
        self.flow.helper_reset();
        self.flow.browser_reset();
        self.phone.screen_on(false, left && was_off).await;
        self.phone.stop_watching();
        self.set_status(|s| {
            s.viewer = None;
            if s.state == State::Connected {
                s.state = State::Ready;
            }
        });
    }
}
