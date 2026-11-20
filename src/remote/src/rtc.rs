//! The second route for the picture: WebRTC, straight from the phone to the browser over UDP.
//!
//! A browser always comes in over its WebSocket (the tunnel or the Wi-Fi door), signed in. Over
//! that WebSocket it offers WebRTC, and this module answers. Once the two ends have found a way to
//! each other, the picture, the pointer, the keys and the acknowledgements go this way; sign-in,
//! quality, the clipboard and the rest stay on the WebSocket. When WebRTC can't connect or drops,
//! the WebSocket has the picture again from the next key frame (core.rs).
//!
//! - **Straight, nothing relayed.** The phone offers its own addresses (Wi-Fi, USB, mobile data:
//!   host candidates) and, for a browser that came through the tunnel, the address its router
//!   shows the internet (server reflexive, asked of a STUN server). The browser does the same, and
//!   ICE tries the pairs. Where none works (strict NATs at both ends, a network that blocks UDP),
//!   the WebSocket keeps the picture. A browser that came through the Wi-Fi door gets no STUN
//!   server: that door never leaves the local network.
//! - **No waiting in the browser.** Each frame carries the playout-delay header extension at 0,
//!   which makes Chrome show a frame the moment it is decoded; the page also sets its receiver's
//!   `jitterBufferTarget` to 0.
//! - **The same pace as the WebSocket.** The browser acknowledges each frame it showed (its RTP
//!   timestamp, from requestVideoFrameCallback) on an unordered data channel that never resends,
//!   so flow.rs keeps its window and sets the bit rate over this route too. A browser that can't
//!   say which frame it showed keeps the WebSocket (app.js). Lost packets are sent again when the
//!   browser asks (NACK, RTX), and a frame it couldn't decode asks for a key frame (PLI).
//! - **No pacer.** str0m's bandwidth estimate and pacer are left off: the pacer sends no faster than
//!   1.1 times the estimate, and on a still desktop (a few packets a second) the estimate sank to a
//!   quarter of a megabit in a test, which would have held a key frame back for over a second. A
//!   frame goes out as soon as it is made, as on the WebSocket.
//! - **Pointer and keys** on an ordered, reliable data channel.
//! - **Only the browser that signed in.** Its offer comes over its signed-in WebSocket, and DTLS
//!   checks the browser's certificate against the fingerprint in that offer.
//!
//! str0m is sans IO: one task per session drives it, with a UDP socket for each address of ours,
//! open only while the session lasts.

use std::collections::{HashMap, VecDeque};
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use std::time::{Duration, Instant};

use str0m::change::SdpOffer;
use str0m::channel::ChannelId;
use str0m::format::{Codec, PayloadParams};
use str0m::media::{Frequency, MediaKind, MediaTime, Mid, Pt};
use str0m::net::{DatagramRecv, Protocol, Receive};
use str0m::rtp::Extension;
use str0m::{Candidate, Event, IceConnectionState, Input, Output, Rtc, RtcConfig};
use tokio::net::UdpSocket;
use tokio::sync::{mpsc, oneshot};
use tokio::task::JoinHandle;

use crate::core::Ev;
use crate::say;

/// How long ICE and DTLS may take before the WebSocket is all there is.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
/// Addresses of ours to offer, at most.
const MAX_LOCAL: usize = 8;
/// Candidates from the browser, at most (a browser has a handful).
const MAX_REMOTE: usize = 32;
/// Frames whose RTP timestamps are kept for the browser's acknowledgements.
const SENT_RING: usize = 256;
/// The biggest offer taken (a browser's is a few kilobytes).
const MAX_OFFER: usize = 64 * 1024;
/// Packets read but not yet handled, at most: past that they are dropped, as a busy network would.
/// The sockets face every network the phone is on, so a flood mustn't grow memory without end.
const MAX_QUEUED: usize = 1024;

/// From the core to a session.
pub enum Cmd {
    /// A frame as the helper made it (Annex B), with its number in the core's count.
    Frame { data: Arc<[u8]>, seq: u32 },
    /// One of the browser's candidates, as its `RTCIceCandidate.candidate` has it.
    Remote(String),
}

/// From a session to the core.
#[derive(Debug, PartialEq)]
pub enum Note {
    /// ICE and DTLS are up: frames can go.
    Up,
    /// A candidate of ours that came later (server reflexive), for the browser.
    Local(String),
    /// The browser lost a frame and asks for a key frame.
    Keyframe,
    /// The browser showed frames up to this one (the core's count).
    Ack(u32),
    /// Pointer or key from the browser, as the WebSocket would bring them.
    Input(Vec<u8>),
    /// Once a second, from the browser's RTCP reports.
    Stats { rtt_ms: Option<u32>, loss: Option<f32>, nacks: u64, plis: u64 },
    /// The session is over, and why.
    Down(String),
}

pub struct Options {
    /// The browser came through the Wi-Fi door: local networks only, and no STUN server.
    pub lan_only: bool,
    /// `host:port` of a STUN server, if one is to be asked.
    pub stun: Option<String>,
}

/// The core's end of one session. Dropping it ends the session.
pub struct Link {
    pub tx: mpsc::UnboundedSender<Cmd>,
}

/// Answer a browser's offer. Ok: the session runs and reports to `events` as `Ev::Rtc { id, gen }`;
/// the answer goes back to the browser. Err: why there is no WebRTC this time.
pub fn start(id: u64, gen: u32, offer: &str, opt: Options, events: mpsc::UnboundedSender<Ev>) -> Result<(Link, String), String> {
    let ips = pick(&interfaces(), opt.lan_only);
    start_on(id, gen, offer, opt, &ips, events)
}

/// `start` on the given addresses of ours (the tests' 127.0.0.1).
pub fn start_on(
    id: u64,
    gen: u32,
    offer: &str,
    opt: Options,
    ips: &[IpAddr],
    events: mpsc::UnboundedSender<Ev>,
) -> Result<(Link, String), String> {
    if offer.len() > MAX_OFFER {
        return Err("the offer is too big".into());
    }
    let offer = SdpOffer::from_sdp_string(offer).map_err(|e| format!("the offer: {e}"))?;
    if ips.is_empty() {
        return Err("the phone is on no network".into());
    }
    let now = Instant::now();
    let mut rtc = RtcConfig::new()
        .clear_codecs()
        .enable_h264(true)
        // Any free number: an offer that has it brings its own, and str0m takes that.
        .set_extension(12, Extension::PlayoutDelay)
        .set_stats_interval(Some(Duration::from_secs(1)))
        .build(now);
    let mut socks = Vec::new();
    for ip in ips.iter().take(MAX_LOCAL) {
        let sock = match std::net::UdpSocket::bind(SocketAddr::new(*ip, 0)) {
            Ok(s) => s,
            Err(e) => {
                say!("webrtc: no socket on {ip}: {e}");
                continue;
            }
        };
        let Ok(local) = sock.local_addr() else { continue };
        if sock.set_nonblocking(true).is_err() {
            continue;
        }
        let Ok(sock) = UdpSocket::from_std(sock) else { continue };
        match Candidate::host(local, "udp") {
            Ok(c) => {
                if rtc.add_local_candidate(c).is_some() {
                    socks.push((local, Arc::new(sock)));
                }
            }
            Err(e) => say!("webrtc: not a candidate, {local}: {e}"),
        }
    }
    if socks.is_empty() {
        return Err("no address of the phone's took a socket".into());
    }
    let answer = rtc.sdp_api().accept_offer(offer).map_err(|e| format!("the offer: {e}"))?;
    let (tx, rx) = mpsc::unbounded_channel();
    let session = Session {
        id,
        gen,
        rtc,
        socks,
        events,
        mid: None,
        pt: None,
        input: None,
        ack: None,
        sent: Sent::default(),
        up: false,
        born: now,
        t0: now,
        last_rtp: 0,
        remote: 0,
        ended: None,
        stun: HashMap::new(),
        stun_to: vec![],
        stun_tries: 0,
        stun_next: None,
        srflx: vec![],
        warned: false,
    };
    let stun = if opt.lan_only { None } else { opt.stun };
    tokio::spawn(session.run(rx, stun));
    Ok((Link { tx }, answer.to_sdp_string()))
}

struct Session {
    id: u64,
    gen: u32,
    rtc: Rtc,
    /// Our sockets, by the address each is bound to (its host candidate).
    socks: Vec<(SocketAddr, Arc<UdpSocket>)>,
    events: mpsc::UnboundedSender<Ev>,
    mid: Option<Mid>,
    pt: Option<Pt>,
    input: Option<ChannelId>,
    ack: Option<ChannelId>,
    sent: Sent,
    up: bool,
    born: Instant,
    t0: Instant,
    last_rtp: u64,
    remote: usize,
    ended: Option<String>,
    /// Our STUN requests in flight: transaction id → the socket it went from.
    stun: HashMap<[u8; 12], usize>,
    stun_to: Vec<SocketAddr>,
    stun_tries: u32,
    stun_next: Option<Instant>,
    srflx: Vec<SocketAddr>,
    warned: bool,
}

impl Session {
    async fn run(mut self, mut cmds: mpsc::UnboundedReceiver<Cmd>, stun: Option<String>) {
        let (ptx, mut prx) = mpsc::channel::<(usize, SocketAddr, Vec<u8>)>(MAX_QUEUED);
        let readers: Vec<JoinHandle<()>> =
            self.socks.iter().enumerate().map(|(i, (_, s))| tokio::spawn(read(i, s.clone(), ptx.clone()))).collect();
        drop(ptx);
        // The STUN server looked up beside the session, not before it.
        let (stx, mut srx) = oneshot::channel::<Vec<SocketAddr>>();
        match stun {
            Some(host) => {
                tokio::spawn(async move {
                    let found = tokio::time::timeout(Duration::from_secs(3), tokio::net::lookup_host(host)).await;
                    let _ = stx.send(found.ok().and_then(|r| r.ok()).map(|a| a.collect()).unwrap_or_default());
                });
            }
            None => drop(stx),
        }
        let mut looking = true;

        let why = loop {
            let timeout = match self.drain() {
                Ok(t) => t,
                Err(e) => break e,
            };
            if let Some(why) = self.ended.take() {
                break why;
            }
            if !self.rtc.is_alive() {
                break "closed".into();
            }
            let now = Instant::now();
            if !self.up && now.duration_since(self.born) > CONNECT_TIMEOUT {
                break "no way through to the browser".into();
            }
            let mut wake = timeout;
            if let Some(t) = self.stun_next {
                wake = wake.min(t);
            }
            if !self.up {
                wake = wake.min(self.born + CONNECT_TIMEOUT + Duration::from_millis(10));
            }
            tokio::select! {
                c = cmds.recv() => match c {
                    Some(c) => self.command(c),
                    None => break "the browser went".into(),
                },
                p = prx.recv() => match p {
                    Some((i, from, data)) => self.packet(i, from, &data),
                    None => break "the network went".into(),
                },
                found = &mut srx, if looking => {
                    looking = false;
                    if let Ok(found) = found {
                        self.stun_to = found;
                        self.stun_send(Instant::now());
                    }
                }
                _ = tokio::time::sleep_until(wake.into()) => {
                    let now = Instant::now();
                    if self.stun_next.is_some_and(|t| now >= t) {
                        self.stun_send(now);
                    }
                    let _ = self.rtc.handle_input(Input::Timeout(now));
                }
            }
        };
        for r in readers {
            r.abort();
        }
        self.rtc.disconnect();
        let _ = self.drain();
        say!("webrtc: ended ({why})");
        self.note(Note::Down(why));
    }

    fn note(&self, note: Note) {
        let _ = self.events.send(Ev::Rtc { id: self.id, gen: self.gen, note });
    }

    /// All str0m has to say, up to its next timeout. Nothing in here changes str0m: a change
    /// must come after a whole drain (str0m's rule), so changes wait for the loop.
    fn drain(&mut self) -> Result<Instant, String> {
        loop {
            match self.rtc.poll_output() {
                Ok(Output::Timeout(t)) => return Ok(t),
                Ok(Output::Transmit(t)) => self.transmit(t.source, t.destination, &t.contents),
                Ok(Output::Event(e)) => self.event(e),
                Err(e) => return Err(format!("WebRTC: {e}")),
            }
        }
    }

    fn transmit(&self, source: SocketAddr, to: SocketAddr, data: &[u8]) {
        if let Some((_, s)) = self.socks.iter().find(|(a, _)| *a == source) {
            // A full socket buffer loses this one packet, as the network might: WebRTC copes.
            let _ = s.try_send_to(data, to);
        }
    }

    fn event(&mut self, e: Event) {
        match e {
            Event::MediaAdded(m) if m.kind == MediaKind::Video => self.mid = Some(m.mid),
            Event::Connected => {
                if !self.up {
                    self.up = true;
                    say!("webrtc: connected in {} ms", self.born.elapsed().as_millis());
                    self.note(Note::Up);
                }
            }
            Event::IceConnectionStateChange(IceConnectionState::Disconnected) if self.up => {
                self.ended = Some("the connection to the browser was lost".into());
            }
            Event::ChannelOpen(id, label) => match label.as_str() {
                "in" => self.input = Some(id),
                "ack" => self.ack = Some(id),
                _ => {}
            },
            Event::ChannelData(d) => {
                if Some(d.id) == self.input && d.binary {
                    self.note(Note::Input(d.data));
                } else if Some(d.id) == self.ack && d.data.len() == 4 {
                    // (A single byte on this channel only keeps the Wi-Fi awake: see app.js.)
                    let ts = u32::from_be_bytes([d.data[0], d.data[1], d.data[2], d.data[3]]);
                    if let Some(seq) = self.sent.find(ts) {
                        self.note(Note::Ack(seq));
                    }
                }
            }
            Event::KeyframeRequest(_) => self.note(Note::Keyframe),
            Event::MediaEgressStats(s) => self.note(Note::Stats {
                rtt_ms: s.rtt.map(|d| d.as_millis() as u32),
                loss: s.loss,
                nacks: s.nacks,
                plis: s.plis,
            }),
            Event::Closed => self.ended = Some("the browser closed it".into()),
            _ => {}
        }
    }

    fn command(&mut self, c: Cmd) {
        match c {
            Cmd::Frame { data, seq } => self.frame(data, seq),
            Cmd::Remote(line) => {
                if self.remote >= MAX_REMOTE {
                    return;
                }
                let line = line.trim().trim_start_matches("a=");
                // A browser's own addresses can come as names (name.local, mDNS) that this side
                // can't look up: those are left out, and its checks to us find it anyway.
                if let Ok(c) = Candidate::from_sdp_string(line) {
                    self.remote += 1;
                    self.rtc.add_remote_candidate(c);
                }
            }
        }
    }

    fn frame(&mut self, data: Arc<[u8]>, seq: u32) {
        if !self.up {
            return;
        }
        let Some(mid) = self.mid else { return };
        let pt = match self.pt {
            Some(p) => p,
            None => {
                let Some(w) = self.rtc.writer(mid) else { return };
                match pick_pt(w.payload_params()) {
                    Some(p) => {
                        self.pt = Some(p);
                        p
                    }
                    None => {
                        if !self.warned {
                            self.warned = true;
                            say!("webrtc: the browser offered no H.264 this phone can send (packetization mode 1)");
                            self.ended = Some("no H.264 in common".into());
                        }
                        return;
                    }
                }
            }
        };
        let now = Instant::now();
        // RTP time from when it goes, at 90 kHz, always forwards: the browser's jitter estimate
        // then sees the network alone.
        let mut rtp = now.duration_since(self.t0).as_micros() as u64 * 9 / 100;
        if rtp <= self.last_rtp {
            rtp = self.last_rtp + 1;
        }
        self.last_rtp = rtp;
        let Some(w) = self.rtc.writer(mid) else { return };
        match w.playout_delay(MediaTime::ZERO, MediaTime::ZERO).write(pt, now, MediaTime::new(rtp, Frequency::NINETY_KHZ), data) {
            Ok(()) => self.sent.push(rtp as u32, seq),
            Err(e) => {
                if !self.warned {
                    self.warned = true;
                    say!("webrtc: a frame wasn't sent: {e}");
                }
            }
        }
    }

    fn packet(&mut self, i: usize, from: SocketAddr, data: &[u8]) {
        let Some((local, _)) = self.socks.get(i) else { return };
        let local = *local;
        if !self.stun.is_empty() {
            if let Some(mapped) = stun_mapped(data, |t| self.stun.contains_key(t)) {
                self.stun.clear();
                self.stun_next = None;
                self.found(local, mapped);
                return;
            }
        }
        let Ok(contents) = DatagramRecv::try_from(data) else { return };
        let input = Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source: from, destination: local, contents });
        // One packet str0m won't take (a stray, a late one) isn't the end of the session.
        let _ = self.rtc.handle_input(input);
    }

    /// The internet's view of one of our addresses, from the STUN server.
    fn found(&mut self, base: SocketAddr, mapped: SocketAddr) {
        if mapped == base || self.srflx.contains(&mapped) {
            return;
        }
        self.srflx.push(mapped);
        let Ok(c) = Candidate::server_reflexive(mapped, base, "udp") else { return };
        if let Some(c) = self.rtc.add_local_candidate(c) {
            let line = c.to_sdp_string();
            say!("webrtc: the internet sees us at {}", masked(mapped));
            self.note(Note::Local(line));
        }
    }

    /// Ask the STUN server for our IPv4 addresses' outside view (IPv6 ones are their own).
    fn stun_send(&mut self, now: Instant) {
        self.stun_next = None;
        let Some(to) = self.stun_to.iter().find(|a| a.is_ipv4()).copied() else { return };
        if self.stun_tries >= 3 || !self.srflx.is_empty() {
            return;
        }
        self.stun_tries += 1;
        for (i, (local, s)) in self.socks.iter().enumerate() {
            if !local.is_ipv4() {
                continue;
            }
            let mut tid = [0u8; 12];
            tid.copy_from_slice(&crate::util::random_bytes(12));
            if s.try_send_to(&stun_request(tid), to).is_ok() {
                self.stun.insert(tid, i);
            }
        }
        self.stun_next = Some(now + Duration::from_millis(400 * self.stun_tries as u64));
    }
}

async fn read(i: usize, sock: Arc<UdpSocket>, tx: mpsc::Sender<(usize, SocketAddr, Vec<u8>)>) {
    let mut buf = vec![0u8; 2048];
    loop {
        match sock.recv_from(&mut buf).await {
            Ok((n, from)) => match tx.try_send((i, from, buf[..n].to_vec())) {
                Ok(()) | Err(mpsc::error::TrySendError::Full(_)) => {}
                Err(mpsc::error::TrySendError::Closed(_)) => return,
            },
            // ICMP for a port that went (Linux reports it on the next read): not the socket's end.
            Err(e) if e.kind() == std::io::ErrorKind::ConnectionRefused => continue,
            Err(_) => return,
        }
    }
}

/// The H.264 the browser offered that this stream can go as: packetization mode 1 (big NAL units
/// in fragments) is a must; constrained baseline, what the phone's x264 makes, is the best.
fn pick_pt<'a>(params: impl Iterator<Item = &'a PayloadParams>) -> Option<Pt> {
    let mut best: Option<(u8, Pt)> = None;
    for p in params {
        let s = p.spec();
        if s.codec != Codec::H264 || s.format.packetization_mode != Some(1) {
            continue;
        }
        let score = h264_score(s.format.profile_level_id.unwrap_or(0x42_00_1f));
        if best.is_none_or(|(b, _)| score > b) {
            best = Some((score, p.pt()));
        }
    }
    best.map(|(_, p)| p)
}

/// Constrained baseline (profile 66 with constraint_set1) 2, other baseline 1, else 0: any
/// decoder of the higher profiles decodes baseline anyway.
fn h264_score(profile_level_id: u32) -> u8 {
    let profile = (profile_level_id >> 16) & 0xff;
    let constraints = (profile_level_id >> 8) & 0xff;
    match (profile, constraints & 0x40 != 0) {
        (0x42, true) => 2,
        (0x42, false) => 1,
        _ => 0,
    }
}

/// The RTP timestamps of the frames sent last, so the browser's acknowledgement (a timestamp) can
/// be turned back into the core's frame number.
#[derive(Default)]
struct Sent {
    ring: VecDeque<(u32, u32)>,
}

impl Sent {
    fn push(&mut self, ts: u32, seq: u32) {
        if self.ring.len() >= SENT_RING {
            self.ring.pop_front();
        }
        self.ring.push_back((ts, seq));
    }

    fn find(&self, ts: u32) -> Option<u32> {
        self.ring.iter().rev().find(|(t, _)| *t == ts).map(|(_, s)| *s)
    }
}

// ── our addresses ─────────────────────────────────────────────────────────

/// Every up interface's addresses, with the interface's name.
fn interfaces() -> Vec<(String, IpAddr)> {
    let mut out = vec![];
    unsafe {
        let mut ifap: *mut libc::ifaddrs = std::ptr::null_mut();
        if libc::getifaddrs(&mut ifap) != 0 {
            return out;
        }
        let mut p = ifap;
        while !p.is_null() {
            let ifa = &*p;
            p = ifa.ifa_next;
            if ifa.ifa_addr.is_null() || ifa.ifa_flags & (libc::IFF_UP as u32) == 0 {
                continue;
            }
            let name = std::ffi::CStr::from_ptr(ifa.ifa_name).to_string_lossy().to_string();
            let ip = match (*ifa.ifa_addr).sa_family as i32 {
                libc::AF_INET => {
                    let sin = &*(ifa.ifa_addr as *const libc::sockaddr_in);
                    IpAddr::from(std::net::Ipv4Addr::from(u32::from_be(sin.sin_addr.s_addr)))
                }
                libc::AF_INET6 => {
                    let sin6 = &*(ifa.ifa_addr as *const libc::sockaddr_in6);
                    IpAddr::from(std::net::Ipv6Addr::from(sin6.sin6_addr.s6_addr))
                }
                _ => continue,
            };
            out.push((name, ip));
        }
        libc::freeifaddrs(ifap);
    }
    out
}

/// The addresses to offer: Wi-Fi first, then wired and USB, then the rest (mobile data). Never
/// loopback, link-local, the 464XLAT stand-in (`clat`) or containers' bridges. `lan_only`: the
/// local networks alone (Wi-Fi, USB, wired).
fn pick(found: &[(String, IpAddr)], lan_only: bool) -> Vec<IpAddr> {
    let mut ranked: Vec<(u8, IpAddr)> = vec![];
    for (name, ip) in found {
        if name == "lo" || ["clat", "dummy", "docker", "veth", "br-", "virbr"].iter().any(|p| name.starts_with(p)) {
            continue;
        }
        let lan = crate::lan::lan_interface(name);
        if lan_only && !lan {
            continue;
        }
        let ok = match ip {
            IpAddr::V4(a) => !(a.is_loopback() || a.is_link_local() || a.is_unspecified() || a.is_multicast() || a.is_broadcast()),
            IpAddr::V6(a) => {
                let first = a.segments()[0];
                // Global (2000::/3) or unique local (fc00::/7); not link-local, loopback, multicast.
                (first & 0xe000) == 0x2000 || (first & 0xfe00) == 0xfc00
            }
        };
        if !ok || ranked.iter().any(|(_, a)| a == ip) {
            continue;
        }
        let rank = if name.starts_with("wlan") {
            0
        } else if name.starts_with("eth") || name.starts_with("en") {
            1
        } else if name.starts_with("usb") {
            2
        } else {
            3
        };
        // IPv4 before IPv6 on the same network: home networks and browsers do IPv4 everywhere.
        let rank = rank * 2 + ip.is_ipv6() as u8;
        ranked.push((rank, *ip));
    }
    ranked.sort_by_key(|(r, _)| *r);
    ranked.into_iter().map(|(_, ip)| ip).take(MAX_LOCAL).collect()
}

/// An address for the log: the network, not the whole address (logs get pasted).
pub fn masked(a: SocketAddr) -> String {
    match a.ip() {
        IpAddr::V4(v) => {
            let o = v.octets();
            format!("{}.{}.x.x", o[0], o[1])
        }
        IpAddr::V6(v) => format!("{:x}:{:x}:…", v.segments()[0], v.segments()[1]),
    }
}

// ── STUN, just enough to ask "how do you see me" ──────────────────────────

const STUN_COOKIE: [u8; 4] = [0x21, 0x12, 0xa4, 0x42];

/// A binding request (RFC 8489) with this transaction id, no attributes.
fn stun_request(tid: [u8; 12]) -> [u8; 20] {
    let mut m = [0u8; 20];
    m[0..2].copy_from_slice(&[0x00, 0x01]);
    m[2..4].copy_from_slice(&[0x00, 0x00]);
    m[4..8].copy_from_slice(&STUN_COOKIE);
    m[8..20].copy_from_slice(&tid);
    m
}

/// The address in a successful binding response to one of our requests (`ours` knows their ids):
/// XOR-MAPPED-ADDRESS, or the old MAPPED-ADDRESS.
fn stun_mapped(buf: &[u8], ours: impl Fn(&[u8; 12]) -> bool) -> Option<SocketAddr> {
    if buf.len() < 20 || buf[0..2] != [0x01, 0x01] || buf[4..8] != STUN_COOKIE {
        return None;
    }
    let mut tid = [0u8; 12];
    tid.copy_from_slice(&buf[8..20]);
    if !ours(&tid) {
        return None;
    }
    let len = u16::from_be_bytes([buf[2], buf[3]]) as usize;
    let body = buf.get(20..20 + len)?;
    let mut plain = None;
    let mut i = 0;
    while i + 4 <= body.len() {
        let kind = u16::from_be_bytes([body[i], body[i + 1]]);
        let n = u16::from_be_bytes([body[i + 2], body[i + 3]]) as usize;
        let v = body.get(i + 4..i + 4 + n)?;
        match kind {
            0x0020 => return address(v, Some((&STUN_COOKIE, &tid))),
            0x0001 => plain = address(v, None),
            _ => {}
        }
        i += 4 + n.div_ceil(4) * 4;
    }
    plain
}

fn address(v: &[u8], xor: Option<(&[u8; 4], &[u8; 12])>) -> Option<SocketAddr> {
    if v.len() < 4 {
        return None;
    }
    let mut port = u16::from_be_bytes([v[2], v[3]]);
    let mut key = [0u8; 16];
    if let Some((cookie, tid)) = xor {
        key[..4].copy_from_slice(cookie);
        key[4..].copy_from_slice(tid);
        port ^= u16::from_be_bytes([cookie[0], cookie[1]]);
    }
    match v[1] {
        0x01 if v.len() >= 8 => {
            let mut o = [0u8; 4];
            for k in 0..4 {
                o[k] = v[4 + k] ^ key[k];
            }
            Some(SocketAddr::new(IpAddr::from(o), port))
        }
        0x02 if v.len() >= 20 => {
            let mut o = [0u8; 16];
            for k in 0..16 {
                o[k] = v[4 + k] ^ key[k];
            }
            Some(SocketAddr::new(IpAddr::from(o), port))
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{Ipv4Addr, Ipv6Addr};

    fn ip(s: &str) -> IpAddr {
        s.parse().unwrap()
    }

    #[test]
    fn addresses_to_offer() {
        let found = vec![
            ("lo".to_string(), ip("127.0.0.1")),
            ("rmnet_data0".to_string(), ip("2001:db8:1:2::5")),
            ("usb0".to_string(), ip("172.16.42.1")),
            ("wlan0".to_string(), ip("fe80::1")),
            ("wlan0".to_string(), ip("192.168.1.20")),
            ("wlan0".to_string(), ip("2001:db8:9:9::20")),
            ("wlan0".to_string(), ip("fd12:3456::20")),
            ("clat".to_string(), ip("192.0.0.1")),
            ("wlan0".to_string(), ip("169.254.3.3")),
            ("docker0".to_string(), ip("172.17.0.1")),
        ];
        assert_eq!(
            pick(&found, false),
            vec![ip("192.168.1.20"), ip("2001:db8:9:9::20"), ip("fd12:3456::20"), ip("172.16.42.1"), ip("2001:db8:1:2::5")],
            "Wi-Fi first (IPv4, then IPv6), USB, mobile data; no loopback, link-local, clat or docker"
        );
        assert_eq!(
            pick(&found, true),
            vec![ip("192.168.1.20"), ip("2001:db8:9:9::20"), ip("fd12:3456::20"), ip("172.16.42.1")],
            "the Wi-Fi door offers no mobile data"
        );
        let many: Vec<_> = (1..=20).map(|i| ("wlan0".to_string(), IpAddr::from(Ipv4Addr::new(10, 0, 0, i)))).collect();
        assert_eq!(pick(&many, false).len(), MAX_LOCAL);
    }

    #[test]
    fn stun_round_trip() {
        let tid = [7u8; 12];
        let req = stun_request(tid);
        assert_eq!(&req[0..2], &[0, 1]);
        assert_eq!(&req[4..8], &STUN_COOKIE);
        assert_eq!(&req[8..20], &tid);

        // A server's answer: XOR-MAPPED-ADDRESS 203.0.113.7:40000, and SOFTWARE before it.
        let reply = |tid: [u8; 12], family: u8, addr: &[u8], port: u16| {
            let mut attr = vec![0, family];
            let p = port ^ 0x2112;
            attr.extend_from_slice(&p.to_be_bytes());
            let mut key = STUN_COOKIE.to_vec();
            key.extend_from_slice(&tid);
            attr.extend(addr.iter().enumerate().map(|(i, b)| b ^ key[i]));
            let mut body = vec![0x80, 0x22, 0, 5, b'h', b'e', b'l', b'l', b'o', 0, 0, 0];
            body.extend_from_slice(&[0x00, 0x20]);
            body.extend_from_slice(&(attr.len() as u16).to_be_bytes());
            body.extend_from_slice(&attr);
            let mut m = vec![0x01, 0x01];
            m.extend_from_slice(&(body.len() as u16).to_be_bytes());
            m.extend_from_slice(&STUN_COOKIE);
            m.extend_from_slice(&tid);
            m.extend_from_slice(&body);
            m
        };
        let v4 = reply(tid, 1, &[203, 0, 113, 7], 40000);
        assert_eq!(stun_mapped(&v4, |t| *t == tid), Some("203.0.113.7:40000".parse().unwrap()));
        assert_eq!(stun_mapped(&v4, |_| false), None, "not one of ours");
        let a6: Ipv6Addr = "2001:db8::9".parse().unwrap();
        let v6 = reply(tid, 2, &a6.octets(), 5000);
        assert_eq!(stun_mapped(&v6, |t| *t == tid), Some(SocketAddr::new(IpAddr::V6(a6), 5000)));
        let mut error = v4.clone();
        error[0..2].copy_from_slice(&[0x01, 0x11]);
        assert_eq!(stun_mapped(&error, |_| true), None, "an error response");
        assert_eq!(stun_mapped(&v4[..30], |_| true), None, "cut short");
        assert_eq!(stun_mapped(&[0u8; 8], |_| true), None);
    }

    #[test]
    fn acknowledged_timestamps() {
        let mut s = Sent::default();
        for i in 0..300u32 {
            s.push((u32::MAX - 100).wrapping_add(i * 3000), i);
        }
        assert_eq!(s.find((u32::MAX - 100).wrapping_add(299 * 3000)), Some(299));
        assert_eq!(s.find((u32::MAX - 100).wrapping_add(100 * 3000)), Some(100));
        assert_eq!(s.find(u32::MAX - 100), None, "older than the ring");
        assert_eq!(s.find(12345), None);
    }

    /// The NAL units of an Annex B stream, without their start codes.
    fn units(b: &[u8]) -> Vec<Vec<u8>> {
        crate::h264::nal_units(b).into_iter().map(|u| u.to_vec()).collect()
    }

    /// A browser played by str0m, offering what the page offers (video to receive, the "in" and
    /// "ack" channels) over real UDP on 127.0.0.1, against a session as the core runs one: frames
    /// arrive whole with the playout delay at 0, an acknowledgement comes back as the core's frame
    /// number, pointer bytes as input, a PLI as a key frame request, and the session ends when the
    /// core lets go of it.
    #[tokio::test(flavor = "current_thread")]
    async fn a_session_end_to_end() {
        use str0m::change::SdpAnswer;
        use str0m::channel::{ChannelConfig, Reliability};
        use str0m::media::{Direction, KeyframeRequestKind};

        let (ev_tx, mut ev_rx) = mpsc::unbounded_channel::<Ev>();
        let sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let addr = sock.local_addr().unwrap();
        let mut b = RtcConfig::new().clear_codecs().enable_h264(true).set_extension(6, Extension::PlayoutDelay).build(Instant::now());
        b.add_local_candidate(Candidate::host(addr, "udp").unwrap());
        let mut api = b.sdp_api();
        let vmid = api.add_media(MediaKind::Video, Direction::RecvOnly, None, None, None);
        let cin = api.add_channel("in".into());
        let cack = api.add_channel_with_config(ChannelConfig {
            label: "ack".into(),
            ordered: false,
            reliability: Reliability::MaxRetransmits { retransmits: 0 },
            ..Default::default()
        });
        let (offer, pending) = api.apply().unwrap();
        let opt = Options { lan_only: true, stun: None };
        let (link, answer) = start_on(1, 7, &offer.to_sdp_string(), opt, &[ip("127.0.0.1")], ev_tx).unwrap();
        assert!(answer.contains("typ host"), "our address is in the answer");
        b.sdp_api().accept_answer(pending, SdpAnswer::from_sdp_string(&answer).unwrap()).unwrap();

        let key: Vec<u8> = [&[0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01][..], &[0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80], &[0, 0, 0, 1, 0x65], &[0x88; 3000]].concat();
        let delta: Vec<u8> = [&[0, 0, 0, 1, 0x41][..], &[0x9a; 700]].concat();
        let pointer = vec![0x10, 1, 0, 100, 0, 50];

        let mut link = Some(link);
        let (mut up, mut open_in, mut open_ack, mut frames_sent) = (false, false, false, false);
        let mut got: Vec<(u32, Vec<u8>, Option<MediaTime>)> = vec![];
        let (mut acked, mut input, mut keyframe, mut down) = (vec![], None, false, None);
        let (mut told_ack, mut told_input, mut asked_key) = (false, false, false);
        let deadline = Instant::now() + Duration::from_secs(20);
        let mut buf = vec![0u8; 2048];
        loop {
            let timeout = loop {
                match b.poll_output().unwrap() {
                    Output::Timeout(t) => break t,
                    Output::Transmit(t) => {
                        let _ = sock.try_send_to(&t.contents, t.destination);
                    }
                    Output::Event(Event::ChannelOpen(id, _)) => {
                        open_in |= id == cin;
                        open_ack |= id == cack;
                    }
                    Output::Event(Event::MediaData(md)) => {
                        got.push((md.time.rebase(Frequency::NINETY_KHZ).numer() as u32, md.data.to_vec(), md.ext_vals.play_delay_max))
                    }
                    Output::Event(_) => {}
                }
            };
            if down.is_some() || Instant::now() > deadline {
                break;
            }
            // The core's side: frames once it's up; the session let go of once all came back.
            if up && open_in && open_ack && !frames_sent {
                frames_sent = true;
                let l = link.as_ref().unwrap();
                l.tx.send(Cmd::Frame { data: Arc::from(&key[..]), seq: 41 }).unwrap();
                l.tx.send(Cmd::Frame { data: Arc::from(&delta[..]), seq: 42 }).unwrap();
            }
            if acked.contains(&42) && input.is_some() && keyframe {
                link = None;
            }
            // The browser's side: one change to str0m, then back to the drain (str0m's rule).
            if got.len() >= 2 && !told_ack {
                told_ack = true;
                let ts = got[1].0;
                b.channel(cack).unwrap().write(true, &ts.to_be_bytes()).unwrap();
                continue;
            }
            if got.len() >= 2 && !told_input {
                told_input = true;
                b.channel(cin).unwrap().write(true, &pointer).unwrap();
                continue;
            }
            if got.len() >= 2 && !asked_key {
                asked_key = true;
                b.writer(vmid).unwrap().request_keyframe(None, KeyframeRequestKind::Pli).unwrap();
                continue;
            }
            let wake = timeout.min(Instant::now() + Duration::from_millis(50));
            tokio::select! {
                r = sock.recv_from(&mut buf) => {
                    let (n, from) = r.unwrap();
                    let contents = DatagramRecv::try_from(&buf[..n]).unwrap();
                    let _ = b.handle_input(Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source: from, destination: addr, contents }));
                }
                ev = ev_rx.recv() => {
                    let Some(Ev::Rtc { id: 1, gen: 7, note }) = ev else { continue };
                    match note {
                        Note::Up => up = true,
                        Note::Ack(seq) => acked.push(seq),
                        Note::Input(d) => input = Some(d),
                        Note::Keyframe => keyframe = true,
                        Note::Down(why) => down = Some(why),
                        _ => {}
                    }
                }
                _ = tokio::time::sleep_until(wake.into()) => {
                    b.handle_input(Input::Timeout(Instant::now())).unwrap();
                }
            }
        }
        assert!(up, "the session came up");
        assert_eq!(got.len(), 2, "both frames arrived");
        assert_eq!(units(&got[0].1), units(&key), "the key frame whole, SPS and PPS with it");
        assert_eq!(units(&got[1].1), units(&delta));
        assert!(got[1].0 > got[0].0, "RTP time goes forwards");
        assert_eq!(got[0].2.map(|t| t.numer()), Some(0), "playout delay 0");
        assert_eq!(acked, vec![42], "the acknowledgement is the core's frame number");
        assert_eq!(input, Some(pointer));
        assert!(keyframe, "a PLI asks for a key frame");
        assert_eq!(down.as_deref(), Some("the browser went"), "letting go of the link ends it");
    }

    #[test]
    fn h264_profiles() {
        assert_eq!(h264_score(0x42e01f), 2);
        assert_eq!(h264_score(0x42c028), 2);
        assert_eq!(h264_score(0x42001f), 1);
        assert_eq!(h264_score(0x64001f), 0);
    }
}
