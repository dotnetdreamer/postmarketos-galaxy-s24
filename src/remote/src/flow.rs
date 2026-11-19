//! The pace between the phone's encoder and a browser across the internet.
//!
//! The helper encodes a frame only while fewer than 4 of its frames are unacknowledged. We hold
//! those acknowledgements back, which is how a frame is never made that the link can't take:
//!
//! - **Frames in flight** to the browser (sent, not yet shown) stay under a window of twice what the
//!   link's own round trip holds at the frame rate, plus two, at least 4 and at most 12: on a 60 ms
//!   link at 30 fps that is 6 frames. Twice, because a frame's round trip also holds the browser's
//!   decoding, which varies; more would only queue somewhere and add delay
//! - **Frame rate** is capped by spacing the acknowledgements (Low 20, Medium 30, High 60 a second)
//! - **Bit rate** follows the link. Frames queueing for two seconds running (the round trip of
//!   P-frames grows past the link's own) cut it by a quarter; a window full most of a second while
//!   the round trip grows cuts it towards what the link really delivered, at most by half a second.
//!   When the link is clear it climbs back 15% every 4 s. Key frames are big by nature and their round trips don't count;
//!   a desktop that is mostly still sends little, which says nothing about the link
//! - A browser that stops acknowledging for 2 s is **stalled**: frames are dropped before they are
//!   sent (the helper keeps going at one a second) until it catches up, and then it gets a key frame
//! - The helper takes 1 s without an acknowledgement as a link that died and came back, and then
//!   encodes freely; so while the window is full it still gets one every 0.8 s, which keeps it to
//!   about a frame a second. Past twice the window a frame isn't sent at all (the browser then waits
//!   for a key frame), so a stall can never fill the link
//!
//! A frame dropped before the encoder costs nothing. One dropped after it would break the chain of
//! frames that refer to it, which is why the dropping happens here by withholding acknowledgements,
//! and why a browser that missed frames waits for the next key frame.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Quality {
    Low,
    Medium,
    High,
}

impl Quality {
    pub fn parse(s: &str) -> Option<Quality> {
        match s {
            "low" => Some(Quality::Low),
            "medium" => Some(Quality::Medium),
            "high" => Some(Quality::High),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Quality::Low => "low",
            Quality::Medium => "medium",
            Quality::High => "high",
        }
    }

    /// The most pixels on the screen's long side.
    pub fn cap(self) -> u32 {
        match self {
            Quality::Low => 1280,
            Quality::Medium => 1920,
            Quality::High => 2560,
        }
    }

    pub fn fps(self) -> u32 {
        match self {
            Quality::Low => 20,
            Quality::Medium => 30,
            Quality::High => 60,
        }
    }

    /// Bit rate: (lowest it goes to, where it starts, highest).
    pub fn rates(self) -> (u32, u32, u32) {
        match self {
            Quality::Low => (200_000, 800_000, 1_200_000),
            Quality::Medium => (400_000, 2_500_000, 4_000_000),
            Quality::High => (800_000, 6_000_000, 12_000_000),
        }
    }
}

/// The screen for a browser window of css_w × css_h, at most `cap` on its long side; width in whole
/// 8s and height even, as KWin's modes and the encoder want them, rounded down so the screen is
/// never wider than the window (the page then shows it pixel for pixel), at least 640 × 400.
/// The helper rounds to the nearest, which leaves these as they are.
pub fn fit(css_w: u32, css_h: u32, cap: u32) -> (u32, u32) {
    let (mut w, mut h) = (css_w.clamp(100, 16384) as f64, css_h.clamp(100, 16384) as f64);
    let k = cap as f64 / w.max(h);
    if k < 1.0 {
        w *= k;
        h *= k;
    }
    let w = ((w / 8.0).floor() as u32 * 8).max(640);
    let h = ((h / 2.0).floor() as u32 * 2).max(400);
    (w, h)
}

/// a ≤ b, for sequence numbers that wrap.
fn seq_le(a: u32, b: u32) -> bool {
    b.wrapping_sub(a) < 1 << 31
}

#[derive(Debug, PartialEq)]
pub enum Release {
    /// Acknowledge one frame to the helper now.
    Now,
    /// Not before then.
    At(Instant),
    /// Nothing to acknowledge, or the window is full (a browser's ack will call again).
    Wait,
}

struct Sent {
    seq: u32,
    at: Instant,
    bytes: usize,
    key: bool,
}

pub struct Flow {
    pub quality: Quality,
    /// Frames the helper sent that we haven't acknowledged to it.
    owed: u32,
    inflight: VecDeque<Sent>,
    last_release: Option<Instant>,
    rtts: VecDeque<(Instant, Duration)>,
    srtt: Option<Duration>,
    delivered: VecDeque<(Instant, usize)>,
    pub bitrate: u32,
    last_change: Instant,
    held: Duration,
    held_since: Option<Instant>,
    started: Instant,
    /// Seconds in a row the round trip said the link is queueing.
    slow_seconds: u32,
}

const STALL: Duration = Duration::from_secs(2);
const RTT_WINDOW: Duration = Duration::from_secs(10);
/// No bit-rate changes this soon after a start: the first key frame and the decoder's set-up.
const WARM_UP: Duration = Duration::from_secs(3);

impl Flow {
    pub fn new(quality: Quality, now: Instant) -> Flow {
        Flow {
            quality,
            owed: 0,
            inflight: VecDeque::new(),
            last_release: None,
            rtts: VecDeque::new(),
            srtt: None,
            delivered: VecDeque::new(),
            bitrate: quality.rates().1,
            last_change: now,
            held: Duration::ZERO,
            held_since: None,
            started: now,
            slow_seconds: 0,
        }
    }

    pub fn set_quality(&mut self, q: Quality, now: Instant) {
        if q != self.quality {
            self.quality = q;
            self.bitrate = q.rates().1;
            self.last_change = now;
        }
    }

    /// A frame came from the helper.
    pub fn frame_in(&mut self) {
        self.owed += 1;
    }

    /// The helper was told hello or bye: it counts from nothing again.
    pub fn helper_reset(&mut self) {
        self.owed = 0;
        self.last_release = None;
    }

    /// The browser is gone or starts over: what was in flight won't be acknowledged.
    pub fn browser_reset(&mut self) {
        self.inflight.clear();
        self.held_since = None;
    }

    pub fn sent(&mut self, seq: u32, bytes: usize, key: bool, now: Instant) {
        self.inflight.push_back(Sent { seq, at: now, bytes, key });
    }

    /// The browser has shown frames up to `seq`.
    pub fn acked(&mut self, seq: u32, now: Instant) {
        while let Some(f) = self.inflight.front() {
            if !seq_le(f.seq, seq) {
                break;
            }
            let f = self.inflight.pop_front().unwrap();
            if f.seq == seq && !f.key {
                let rtt = now.saturating_duration_since(f.at);
                self.rtts.push_back((now, rtt));
                self.srtt = Some(match self.srtt {
                    Some(s) => (s * 7 + rtt) / 8,
                    None => rtt,
                });
            }
            self.delivered.push_back((now, f.bytes));
        }
        while self.rtts.front().is_some_and(|(t, _)| now.duration_since(*t) > RTT_WINDOW) {
            self.rtts.pop_front();
        }
    }

    pub fn min_rtt(&self) -> Duration {
        self.rtts.iter().map(|(_, r)| *r).min().unwrap_or(Duration::from_millis(60))
    }

    pub fn srtt(&self) -> Duration {
        self.srtt.unwrap_or_else(|| self.min_rtt())
    }

    pub fn window(&self) -> usize {
        let frames = (2.0 * self.min_rtt().as_secs_f64() * self.quality.fps() as f64).ceil() as usize;
        (frames + 2).clamp(4, 12)
    }

    pub fn in_flight(&self) -> usize {
        self.inflight.len()
    }

    /// Too many in flight to send another: the browser is that far behind.
    pub fn overfull(&self) -> bool {
        self.inflight.len() >= 2 * self.window()
    }

    pub fn stalled(&self, now: Instant) -> bool {
        self.inflight.front().is_some_and(|f| now.duration_since(f.at) > STALL)
    }

    /// Whether to acknowledge a frame to the helper now. `slow`: nobody is watching (no browser,
    /// a hidden tab, a stalled link): one a second keeps the helper from thinking the link died.
    pub fn release(&mut self, now: Instant, slow: bool) -> Release {
        if self.owed == 0 {
            return Release::Wait;
        }
        if !slow && self.inflight.len() >= self.window() {
            self.held_since.get_or_insert(now);
            // Not a whole second without one, or the helper stops waiting (see the top).
            let keep_alive = Duration::from_millis(800);
            match self.last_release {
                Some(last) if now < last + keep_alive => return Release::At(last + keep_alive),
                _ => {
                    self.owed -= 1;
                    self.last_release = Some(now);
                    return Release::Now;
                }
            }
        }
        if let Some(h) = self.held_since.take() {
            self.held += now.duration_since(h);
        }
        let gap = if slow {
            Duration::from_secs(1)
        } else {
            Duration::from_secs(1) / self.quality.fps()
        };
        if let Some(last) = self.last_release {
            if now < last + gap {
                return Release::At(last + gap);
            }
        }
        self.owed -= 1;
        self.last_release = Some(now);
        Release::Now
    }

    /// Once a second: a new bit rate for the encoder, if it should change.
    pub fn adjust(&mut self, now: Instant) -> Option<u32> {
        while self.delivered.front().is_some_and(|(t, _)| now.duration_since(*t) > Duration::from_secs(2)) {
            self.delivered.pop_front();
        }
        let delivered_bps = self.delivered.iter().map(|(_, b)| *b as u64).sum::<u64>() * 8 / 2;
        let mut held = self.held;
        if let Some(h) = self.held_since {
            held += now.duration_since(h);
            self.held_since = Some(now);
        }
        self.held = Duration::ZERO;
        let min_rtt = self.min_rtt();
        let queue = self.srtt().saturating_sub(min_rtt);
        let queueing = queue > min_rtt.max(Duration::from_millis(100));
        let filling = queue > (min_rtt / 2).max(Duration::from_millis(40));
        self.slow_seconds = if queueing { self.slow_seconds + 1 } else { 0 };
        // Waiting on the window most of the second while the round trip grows: the link limits.
        let full = held > Duration::from_millis(400) && filling;
        if now.duration_since(self.started) < WARM_UP {
            return None;
        }
        let (lo, _, hi) = self.quality.rates();
        let since = now.duration_since(self.last_change);
        let next = if (full || self.slow_seconds >= 2) && since >= Duration::from_secs(1) {
            let mut cut = self.bitrate as u64 * 3 / 4;
            // What got through is what the link carries; no more than half off at a time, in case
            // it was the picture, not the link, that sent little.
            if full {
                cut = cut.min(delivered_bps * 9 / 10).max(self.bitrate as u64 / 2);
            }
            cut.max(lo as u64) as u32
        } else if !full && !queueing && queue < Duration::from_millis(30) && since >= Duration::from_secs(4) && self.bitrate < hi {
            ((self.bitrate as u64 * 115 / 100) as u32).min(hi)
        } else {
            return None;
        };
        let change = (next as f64 - self.bitrate as f64).abs() / self.bitrate as f64;
        if change < 0.05 {
            return None;
        }
        self.bitrate = next;
        self.last_change = now;
        self.slow_seconds = 0;
        Some(next)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: Duration = Duration::from_millis(1);

    #[test]
    fn fit_caps_and_rounds() {
        assert_eq!(fit(1920, 1080, 1920), (1920, 1080));
        assert_eq!(fit(1920, 1080, 1280), (1280, 720));
        assert_eq!(fit(2560, 1440, 1920), (1920, 1080));
        assert_eq!(fit(1366, 657, 1920), (1360, 656));
        assert_eq!(fit(1366, 768, 1920), (1360, 768));
        assert_eq!(fit(400, 300, 1920), (640, 400));
    }

    #[test]
    fn paces_to_the_frame_rate() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        for _ in 0..3 {
            f.frame_in();
        }
        assert_eq!(f.release(t, false), Release::Now);
        // 30 fps: the next one 33 ms later.
        assert_eq!(f.release(t + MS, false), Release::At(t + Duration::from_secs(1) / 30));
        assert_eq!(f.release(t + 34 * MS, false), Release::Now);
        assert_eq!(f.release(t + 70 * MS, false), Release::Now);
        assert_eq!(f.release(t + 200 * MS, false), Release::Wait, "nothing owed");
    }

    #[test]
    fn window_holds_frames_until_acked() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        for seq in 1..=6 {
            f.sent(seq, 1000, false, t);
        }
        f.acked(1, t + 50 * MS); // a 50 ms link at 30 fps: 3 frames, twice that, + 2
        assert_eq!(f.window(), 5);
        for _ in 0..3 {
            f.frame_in();
        }
        assert_eq!(f.release(t + 50 * MS, false), Release::Now, "5 in flight is full, but nothing for 0.8 s before");
        // Full: only a keep-alive, 0.8 s after the last, or the helper stops waiting and floods.
        assert_eq!(f.release(t + 60 * MS, false), Release::At(t + 850 * MS));
        f.acked(3, t + 100 * MS); // 3 in flight: room again, at the frame rate's pace
        assert_eq!(f.release(t + 100 * MS, false), Release::Now);
        assert!(!f.overfull());
        for seq in 7..=13 {
            f.sent(seq, 1000, false, t + 150 * MS);
        }
        assert!(f.overfull(), "twice the window: no more frames go");
    }

    #[test]
    fn rtt_sets_the_window() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        assert_eq!(f.window(), 6, "no round trip yet: 60 ms is assumed");
        for seq in 1..=3u32 {
            f.sent(seq, 1000, false, t);
            f.acked(seq, t + 100 * MS);
        }
        assert_eq!(f.min_rtt(), 100 * MS);
        assert_eq!(f.window(), 8); // 0.1 s × 30 = 3, twice is 6, + 2
        let mut g = Flow::new(Quality::High, t);
        g.sent(1, 1000, false, t);
        g.acked(1, t + 300 * MS);
        assert_eq!(g.window(), 12, "at most 12");
    }

    #[test]
    fn stalls_after_two_seconds() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        f.sent(1, 1000, false, t);
        assert!(!f.stalled(t + 1900 * MS));
        assert!(f.stalled(t + 2100 * MS));
        // Slow release while stalled: one a second.
        f.frame_in();
        f.frame_in();
        assert_eq!(f.release(t + 2100 * MS, true), Release::Now);
        assert_eq!(f.release(t + 2200 * MS, true), Release::At(t + 3100 * MS));
    }

    #[test]
    fn sequence_numbers_wrap() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Low, t);
        f.sent(u32::MAX, 10, false, t);
        f.sent(0, 10, false, t);
        f.sent(1, 10, false, t);
        f.acked(0, t + 50 * MS);
        assert_eq!(f.in_flight(), 1);
    }

    /// Frames sent every 100 ms, each acknowledged `rtt` ms later.
    fn feed(f: &mut Flow, from: u32, to: u32, mut now: Instant, rtt: u32, bytes: usize) -> Instant {
        for seq in from..to {
            f.sent(seq, bytes, false, now);
            f.acked(seq, now + rtt * MS);
            now += 100 * MS;
        }
        now
    }

    #[test]
    fn a_still_desktop_cuts_nothing() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        // Little sent, quick acks: nothing says the link is the limit.
        let now = feed(&mut f, 0, 60, t, 40, 300);
        for s in 0..3 {
            let r = f.adjust(now + s * Duration::from_secs(1));
            assert!(r.is_none() || r.unwrap() > Quality::Medium.rates().1, "{r:?}");
        }
    }

    #[test]
    fn key_frames_and_the_start_dont_count() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        // A big first key frame, slow to arrive and decode.
        f.sent(1, 120_000, true, t);
        f.acked(1, t + 900 * MS);
        assert_eq!(f.srtt, None, "a key frame's round trip isn't sampled");
        let now = feed(&mut f, 2, 12, t + 900 * MS, 40, 2000);
        assert_eq!(f.adjust(now), None, "in the warm-up");
        assert!(f.adjust(t + 4 * Duration::from_secs(1)).is_none_or(|b| b > Quality::Medium.rates().1));
    }

    #[test]
    fn queueing_for_two_seconds_cuts_a_quarter() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        let start = f.bitrate;
        let now = feed(&mut f, 0, 10, t, 40, 5000); // the link's own: 40 ms
        let now = feed(&mut f, 10, 40, now, 400, 5000); // then 400 ms: queueing
        assert_eq!(f.adjust(now), None, "one second isn't enough");
        let cut = f.adjust(now + Duration::from_secs(1)).expect("a cut after two");
        assert_eq!(cut, start / 4 * 3);
    }

    #[test]
    fn a_full_window_cuts_to_what_was_delivered() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::High, t);
        // 1 Mbit/s really delivered over the last 2 s, the round trip growing from 40 to 150 ms ...
        let now = feed(&mut f, 0, 10, t + Duration::from_secs(4), 40, 12_500);
        let now = feed(&mut f, 10, 20, now, 150, 12_500);
        // ... and the window full for the whole of the last second.
        for seq in 100..110 {
            f.sent(seq, 12_500, false, now);
        }
        f.frame_in();
        f.frame_in();
        assert_eq!(f.release(now, false), Release::Now, "full, so only the keep-alive");
        assert_eq!(f.release(now + 10 * MS, false), Release::At(now + 800 * MS));
        let cut = f.adjust(now + Duration::from_secs(1)).expect("a cut");
        assert_eq!(cut, 3_000_000, "towards the 0.9 Mbit/s delivered, but at most half off");
        let again = f.adjust(now + Duration::from_secs(2)).expect("and again");
        assert_eq!(again, 1_500_000);
    }

    #[test]
    fn climbs_back_when_clear() {
        let t = Instant::now();
        let mut f = Flow::new(Quality::Medium, t);
        f.bitrate = 1_000_000;
        let now = feed(&mut f, 0, 50, t, 40, 1000);
        let up = f.adjust(now).expect("a raise");
        assert_eq!(up, 1_150_000);
        assert_eq!(f.adjust(now + Duration::from_secs(1)), None, "not again for 4 s");
        assert_eq!(f.adjust(now + Duration::from_secs(5)), Some(1_322_500));
    }
}
