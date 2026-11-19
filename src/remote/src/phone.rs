//! The phone's own screen while a browser is connected.
//!
//! KWin has one screen-off state for every screen it drives (Workspace::requestDpmsState): the
//! browser's virtual screen and the phone's panel go dark together, and the first key or pointer
//! move from either side lights both. Left alone, the phone in a pocket would light up with the
//! desktop on it whenever the browser is used. So while a browser is connected:
//!
//! - The panel is switched off as a screen (kscreen-doctor output.DSI-1.disable): KWin moves
//!   everything to the browser's screen, which stays lit. KWin keeps outputs per set of screens
//!   (kwinoutputconfig.json), so when the browser's screen goes, however that happens, the panel
//!   comes back by itself (we switch it on anyway); and when the browser's screen comes again, KWin
//!   switches the panel off by itself, from that memory. A panel found off beside the browser's
//!   screen is taken as ours, then; and when the browser wants the phone's screen on, it is switched
//!   on whoever turned it off
//! - The touchscreen is grabbed (EVIOCGRAB): a touch on a switched-off panel would land at KWin's
//!   (0, 0), on the browser's screen. KWin's own "disable this device" is saved in kcminputrc and
//!   would outlive a crash; a grab ends with the process that holds it
//! - The power key gives the phone back: a press locks the session, switches the panel on and lets
//!   go of the touch; the next press takes the panel away again while the browser's screen stays lit
//!   (Plasma's own answer to the key, all screens off, would leave the browser's dark)
//! - When the browser has gone for good, the phone is locked and its screen turned off, the way it
//!   was in the pocket
//!
//! All of it is undone in that order: lock, panel on, screens off, touch let go.

use std::ffi::CString;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::PathBuf;

use tokio::io::unix::AsyncFd;
use tokio::process::Command;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::core::Ev;
use crate::say;

const KEY_POWER: u16 = 116;
const EV_KEY: u16 = 1;
// _IOW('E', 0x90, int)
const EVIOCGRAB: libc::c_ulong = 0x4004_4590;

/// kscreen-doctor, as the session would run it.
pub async fn kscreen(args: &[&str]) -> Result<String, String> {
    let out = Command::new("kscreen-doctor")
        .args(args)
        .env("QT_QPA_PLATFORM", "wayland")
        .output()
        .await
        .map_err(|e| format!("kscreen-doctor: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    if out.status.success() {
        Ok(text)
    } else {
        Err(format!(
            "kscreen-doctor {} failed: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim()
        ))
    }
}

/// The phone's panel as KWin knows it: its name and whether it is on.
pub async fn panel() -> Option<(String, bool)> {
    let json = kscreen(&["-j"]).await.ok()?;
    let v: serde_json::Value = serde_json::from_str(&json).ok()?;
    for o in v.get("outputs")?.as_array()? {
        let name = o.get("name")?.as_str()?;
        let internal = ["DSI", "eDP", "LVDS"].iter().any(|p| name.starts_with(p));
        if internal {
            let enabled = o.get("enabled").and_then(|e| e.as_bool()).unwrap_or(true);
            return Some((name.to_string(), enabled));
        }
    }
    None
}

/// The evdev node of the first input device whose name `pick` likes.
pub fn find_input(pick: impl Fn(&str) -> bool) -> Option<PathBuf> {
    let mut nodes: Vec<_> = std::fs::read_dir("/sys/class/input")
        .ok()?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.starts_with("event"))
        .collect();
    nodes.sort_by_key(|n| n[5..].parse::<u32>().unwrap_or(u32::MAX));
    nodes.into_iter().find_map(|n| {
        let name = std::fs::read_to_string(format!("/sys/class/input/{n}/device/name")).ok()?;
        pick(name.trim()).then(|| PathBuf::from(format!("/dev/input/{n}")))
    })
}

fn open_input(path: &PathBuf) -> std::io::Result<OwnedFd> {
    let c = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
    let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDONLY | libc::O_NONBLOCK | libc::O_CLOEXEC) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

/// The touchscreen, held so that nothing else gets its touches; let go when dropped.
pub struct Grab(OwnedFd);

impl Grab {
    pub fn take() -> Result<Grab, String> {
        let path = find_input(|n| n.to_lowercase().contains("touchscreen"))
            .ok_or("no touchscreen found")?;
        let fd = open_input(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        let r = unsafe { libc::ioctl(fd.as_raw_fd(), EVIOCGRAB as _, 1 as libc::c_int) };
        if r != 0 {
            return Err(format!("grabbing {}: {}", path.display(), std::io::Error::last_os_error()));
        }
        Ok(Grab(fd))
    }
}

impl Drop for Grab {
    fn drop(&mut self) {
        unsafe { libc::ioctl(self.0.as_raw_fd(), EVIOCGRAB as _, 0 as libc::c_int) };
    }
}

/// Presses of the power key, as Ev::PowerKey, until the task is dropped.
fn watch_power_key(events: mpsc::UnboundedSender<Ev>) -> Option<JoinHandle<()>> {
    let path = find_input(|n| n.contains("power-keys") || n.contains("pwrkey") || n == "Power Button")?;
    let fd = open_input(&path).ok()?;
    // We own the descriptor (OwnedFd, moved in): new's I/O-safety caveat doesn't apply.
    #[allow(deprecated)]
    let afd = AsyncFd::new(fd).ok()?;
    Some(tokio::spawn(async move {
        let mut buf = [0u8; 24 * 16];
        loop {
            let Ok(mut ready) = afd.readable().await else { return };
            let n = unsafe { libc::read(afd.as_raw_fd(), buf.as_mut_ptr() as *mut _, buf.len()) };
            if n < 0 {
                if std::io::Error::last_os_error().kind() == std::io::ErrorKind::WouldBlock {
                    ready.clear_ready();
                    continue;
                }
                return;
            }
            // struct input_event on 64-bit: 16 bytes of time, then type u16, code u16, value s32.
            for e in buf[..n as usize].chunks_exact(24) {
                let ty = u16::from_ne_bytes([e[16], e[17]]);
                let code = u16::from_ne_bytes([e[18], e[19]]);
                let value = i32::from_ne_bytes([e[20], e[21], e[22], e[23]]);
                if ty == EV_KEY && code == KEY_POWER && value == 1 && events.send(Ev::PowerKey).is_err() {
                    return;
                }
            }
        }
    }))
}

/// Lock the session (kscreenlocker), as the power key would. Not when it is locked already:
/// kscreenlocker then only signals its lock screen process (`kill(pid, SIGUSR1)`), and if that process
/// has died (its emergency mode) the pid is 0, and the signal goes to KWin's whole process group.
pub async fn lock(bus: &zbus::Connection) {
    if locked(bus).await == Some(true) {
        return;
    }
    let r = bus
        .call_method(
            Some("org.freedesktop.ScreenSaver"),
            "/ScreenSaver",
            Some("org.freedesktop.ScreenSaver"),
            "Lock",
            &(),
        )
        .await;
    if let Err(e) = r {
        say!("phone: couldn't lock: {e}");
    }
}

/// Whether the session is locked. (logind's LockedHint is wrong on Plasma Mobile here.)
pub async fn locked(bus: &zbus::Connection) -> Option<bool> {
    let r = bus
        .call_method(
            Some("org.freedesktop.ScreenSaver"),
            "/ScreenSaver",
            Some("org.freedesktop.ScreenSaver"),
            "GetActive",
            &(),
        )
        .await
        .ok()?;
    r.body().deserialize::<bool>().ok()
}

fn marker() -> PathBuf {
    crate::util::cache_dir().join("phone-screen-off")
}

pub struct Phone {
    bus: zbus::Connection,
    events: mpsc::UnboundedSender<Ev>,
    /// The panel we switched off, while it is off.
    off: Option<String>,
    grab: Option<Grab>,
    power: Option<JoinHandle<()>>,
}

impl Phone {
    pub fn new(bus: zbus::Connection, events: mpsc::UnboundedSender<Ev>) -> Phone {
        Phone { bus, events, off: None, grab: None, power: None }
    }

    pub fn is_off(&self) -> bool {
        self.off.is_some()
    }

    /// The panel off and its touch held. Err: nothing was changed, and why.
    pub async fn screen_off(&mut self) -> Result<(), String> {
        if self.off.is_some() {
            return Ok(());
        }
        let Some((name, enabled)) = panel().await else {
            return Err("no phone panel among KWin's screens".into());
        };
        // Touch first: from the moment the panel is off, a touch would land on the browser's screen.
        match Grab::take() {
            Ok(g) => self.grab = Some(g),
            Err(e) => say!("phone: touch not held ({e}); touches may reach the desktop"),
        }
        let _ = crate::util::write_atomic(&marker(), &name);
        if enabled {
            if let Err(e) = kscreen(&[&format!("output.{name}.disable")]).await {
                self.grab = None;
                let _ = std::fs::remove_file(marker());
                return Err(e);
            }
            say!("phone: {name} off while the browser is connected");
        } else {
            say!("phone: {name} was off already (KWin remembers it beside the browser's screen); ours now");
        }
        self.off = Some(name);
        if self.power.is_none() {
            self.power = watch_power_key(self.events.clone());
            if self.power.is_none() {
                say!("phone: no power key found to give the phone back with");
            }
        }
        Ok(())
    }

    /// The power key no longer watched: the browser's session is over.
    pub fn stop_watching(&mut self) {
        if let Some(p) = self.power.take() {
            p.abort();
        }
    }

    /// The panel back. `lock` locks the session first; `dark` then turns the screens off, so the
    /// phone is as it was in a pocket.
    pub async fn screen_on(&mut self, lock_it: bool, dark: bool) {
        let Some(name) = self.off.take() else {
            self.grab = None;
            return;
        };
        if lock_it {
            lock(&self.bus).await;
        }
        match kscreen(&[&format!("output.{name}.enable")]).await {
            Ok(_) => say!("phone: {name} back on{}", if lock_it { ", locked" } else { "" }),
            Err(e) => say!("phone: {e}"),
        }
        if dark {
            if let Err(e) = kscreen(&["--dpms", "off"]).await {
                say!("phone: {e}");
            }
        }
        self.grab = None;
        let _ = std::fs::remove_file(marker());
    }
}

impl Phone {
    /// The panel on, as the browser asks: ours to give back, or off by KWin's memory of a session.
    pub async fn ensure_on(&mut self) {
        if self.off.is_some() {
            self.screen_on(false, false).await;
            return;
        }
        if let Some((name, false)) = panel().await {
            match kscreen(&[&format!("output.{name}.enable")]).await {
                Ok(_) => say!("phone: {name} on, as the browser asks"),
                Err(e) => say!("phone: {e}"),
            }
        }
    }
}

/// At login, after a run that ended without giving the phone its screen back (a power cut, say):
/// KWin brings the panel back by itself when the browser's screen is gone, but check.
pub async fn cleanup() {
    let Ok(name) = std::fs::read_to_string(marker()) else { return };
    let name = name.trim().to_string();
    if let Some((panel, false)) = panel().await {
        if panel == name {
            match kscreen(&[&format!("output.{name}.enable")]).await {
                Ok(_) => say!("cleanup: {name} was left off, back on"),
                Err(e) => say!("cleanup: {e}"),
            }
        }
    }
    let _ = std::fs::remove_file(marker());
}
