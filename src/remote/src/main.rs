//! s24-remote: the phone's desktop in any browser, from anywhere.
//!
//!   s24-remote on | off | toggle     what the Remote Desktop quick setting runs. On stays on
//!                                    through a restart, until it is turned off
//!   s24-remote status [--json]       on or off, the addresses and the code
//!   s24-remote show                  the notification with the address and code, again
//!   s24-remote password [--remove]   the owner's password, set at the phone (password.rs)
//!   s24-remote serve                 the program itself (what `on` starts, in the background)
//!   s24-remote login                 at login: the phone's screen back if a run left it off, and
//!                                    Remote Desktop on again if it was on
//!   s24-remote cleanup               the first half of that only
//!
//! While it is on: a tunnel to the internet (tunnel.rs) and a door on the phone's Wi-Fi address
//! (lan.rs), both to the same sign-in that opens to the code shown on the phone or the owner's
//! password (http.rs, auth.rs); once a browser is in, a screen of its own on the phone's desktop in
//! docked mode, encoded as H.264 by Open Android DeX's postmarketOS helper and paced to the
//! browser's link (core.rs, flow.rs). The phone stays awake while it is on (logind), and its own
//! screen is off while a browser is connected (phone.rs) unless asked for.

mod auth;
mod config;
mod core;
mod dbus;
mod flow;
mod h264;
mod helper;
mod http;
mod lan;
mod panels;
mod password;
mod phone;
mod rtc;
mod status;
mod tunnel;
mod util;
mod www;

use std::os::unix::process::CommandExt;
use std::process::{Command, ExitCode, Stdio};
use std::sync::atomic::AtomicU64;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::signal::unix::{signal, SignalKind};
use tokio::sync::{mpsc, oneshot, watch};

use crate::auth::{spaced, Auth};
use crate::config::{Config, Tunnel};
use crate::status::{State, Status};
use crate::tunnel::Way;

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn pid_file() -> std::path::PathBuf {
    util::runtime_dir().join("pid")
}

fn status_file() -> std::path::PathBuf {
    util::runtime_dir().join("status.json")
}

/// There while Remote Desktop is to stay on: `on` makes it, `off` (or Turn off) takes it away, and
/// the login autostart (and the phone's automatic login, see autologin) start it again while it is.
fn keep_on_file() -> std::path::PathBuf {
    util::state_dir().join("keep-on")
}

fn keep_on(on: bool) {
    let f = keep_on_file();
    if on {
        let _ = util::write_atomic(&f, "Remote Desktop stays on through a restart: `s24-remote off` ends it\n");
    } else {
        let _ = std::fs::remove_file(f);
    }
}

/// The running copy's pid, if there is one (and it really is us).
fn running_pid() -> Option<i32> {
    let pid: i32 = std::fs::read_to_string(pid_file()).ok()?.trim().parse().ok()?;
    let cmdline = std::fs::read(format!("/proc/{pid}/cmdline")).ok()?;
    let args: Vec<&[u8]> = cmdline.split(|b| *b == 0).collect();
    let ours = args.first().is_some_and(|a| a.ends_with(b"s24-remote")) && args.get(1) == Some(&&b"serve"[..]);
    ours.then_some(pid)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let code = match args.get(1).map(String::as_str).unwrap_or("status") {
        "serve" => tokio_main(serve()),
        "on" => {
            keep_on(true);
            on()
        }
        "off" => {
            keep_on(false);
            off()
        }
        "toggle" => {
            if running_pid().is_some() {
                keep_on(false);
                off()
            } else {
                keep_on(true);
                on()
            }
        }
        "login" => tokio_main(login()),
        "password" => set_password(args.get(2).map(String::as_str) == Some("--remove")),
        "status" => status(args.get(2).map(String::as_str) == Some("--json")),
        "show" => match running_pid() {
            Some(pid) => {
                unsafe { libc::kill(pid, libc::SIGUSR1) };
                0
            }
            None => {
                eprintln!("Remote Desktop is off");
                1
            }
        },
        "cleanup" => tokio_main(async {
            util::log_open();
            if running_pid().is_none() {
                phone::cleanup().await;
            }
            0
        }),
        "version" | "--version" => {
            println!("s24-remote {VERSION}");
            0
        }
        _ => {
            eprintln!("usage: s24-remote on | off | toggle | status [--json] | show | password [--remove] | serve | login | cleanup | version");
            2
        }
    };
    ExitCode::from(code as u8)
}

fn tokio_main(f: impl std::future::Future<Output = i32>) -> i32 {
    tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio").block_on(f)
}

fn on() -> i32 {
    if running_pid().is_some() {
        return status(false);
    }
    let Ok(exe) = std::env::current_exe() else { return 1 };
    let _ = std::fs::remove_file(status_file());
    let spawned = unsafe {
        Command::new(exe)
            .arg("serve")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .pre_exec(|| {
                libc::setsid();
                Ok(())
            })
            .spawn()
    };
    if let Err(e) = spawned {
        eprintln!("couldn't start: {e}");
        return 1;
    }
    for _ in 0..50 {
        std::thread::sleep(Duration::from_millis(100));
        if status_file().exists() {
            return status(false);
        }
    }
    eprintln!("Remote Desktop didn't start: see {}", util::cache_dir().join("s24-remote.log").display());
    1
}

fn off() -> i32 {
    let Some(pid) = running_pid() else {
        println!("off");
        return 0;
    };
    unsafe { libc::kill(pid, libc::SIGTERM) };
    for _ in 0..150 {
        std::thread::sleep(Duration::from_millis(100));
        if !std::path::Path::new(&format!("/proc/{pid}")).exists() {
            println!("off");
            return 0;
        }
    }
    eprintln!("still stopping (pid {pid})");
    1
}

fn status(as_json: bool) -> i32 {
    let text = if running_pid().is_some() { std::fs::read_to_string(status_file()).ok() } else { None };
    let Some(text) = text else {
        println!("{}", if as_json { r#"{"state":"off"}"# } else { "off" });
        return 0;
    };
    if as_json {
        println!("{text}");
        return 0;
    }
    let v: serde_json::Value = serde_json::from_str(&text).unwrap_or_default();
    println!("on: {}", v["summary"].as_str().unwrap_or(""));
    if let Some(u) = v["url"].as_str() {
        println!("address: {u}");
    }
    if let Some(u) = v["lan"].as_str() {
        println!("on this Wi-Fi: {u}");
    }
    if let Some(c) = v["code"].as_str() {
        println!("code: {}", spaced(c));
    }
    if let Some(w) = v["viewer"].as_str() {
        println!("browser: {w}");
    }
    0
}

/// The text of the notification that hands out the address.
fn note_text(s: &Status) -> (String, String) {
    let code = spaced(&s.code);
    let wifi = s.lan_url.as_deref();
    if s.locked_secs > 0 {
        return (
            "Remote Desktop: sign-in closed".into(),
            "Too many wrong codes were typed. Turn Remote Desktop off and on to open it again.".into(),
        );
    }
    match (&s.state, &s.url, wifi) {
        (_, Some(url), Some(l)) => (
            "Remote Desktop is on".into(),
            format!("Open {url} in any browser (on this Wi-Fi: {l}) and type the code {code}"),
        ),
        (_, Some(url), None) => ("Remote Desktop is on".into(), format!("Open {url} in any browser and type the code {code}")),
        (State::Error, None, Some(l)) => (
            "Remote Desktop: on this Wi-Fi only".into(),
            format!("Open {l} and type the code {code}. Not on the internet: {}", s.message),
        ),
        (_, None, Some(l)) => (
            "Remote Desktop is on".into(),
            format!("On this Wi-Fi: open {l} and type the code {code}. Getting an address on the internet…"),
        ),
        (State::Error, None, None) => ("Remote Desktop can't be reached".into(), s.message.clone()),
        (_, None, None) => ("Remote Desktop is starting".into(), "Getting an address on the internet…".into()),
    }
}

/// At login (the autostart): the phone's screen back if a run left it off; Remote Desktop on again
/// if it is to stay on. After the phone's automatic login (autologin) the session must be locked:
/// kscreenlocker's LockOnStart does it, and this makes sure.
async fn login() -> i32 {
    util::log_open();
    if running_pid().is_none() {
        phone::cleanup().await;
    }
    if std::env::var_os("S24_REMOTE_AUTOLOGIN").is_some() {
        if let Ok(bus) = zbus::Connection::session().await {
            if phone::locked(&bus).await != Some(true) {
                say!("login: an automatic login that isn't locked: locking it");
                phone::lock(&bus).await;
            }
        }
    }
    if keep_on_file().exists() && running_pid().is_none() {
        say!("login: Remote Desktop was on: on again");
        return on();
    }
    0
}

/// `s24-remote password`: the owner's password, typed twice at the phone (not shown).
fn set_password(remove: bool) -> i32 {
    if remove {
        return match password::remove() {
            Ok(()) => {
                println!("The password is gone: only the code shown on the phone signs in now");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        };
    }
    let first = read_hidden("New password: ");
    if let Err(e) = password::check_new(&first) {
        eprintln!("{e}");
        return 1;
    }
    if read_hidden("Again: ") != first {
        eprintln!("They don't match: the password is as it was");
        return 1;
    }
    match password::set(&first) {
        Ok(()) => {
            println!("Set: a browser can sign in with it, or with the code shown on the phone");
            0
        }
        Err(e) => {
            eprintln!("{e}");
            1
        }
    }
}

/// A line from the terminal with echo off (from stdin as it is, when that isn't a terminal).
fn read_hidden(prompt: &str) -> String {
    use std::io::{BufRead, Write};
    eprint!("{prompt}");
    let _ = std::io::stderr().flush();
    let fd = 0;
    let mut old: libc::termios = unsafe { std::mem::zeroed() };
    let tty = unsafe { libc::tcgetattr(fd, &mut old) } == 0;
    if tty {
        let mut quiet = old;
        quiet.c_lflag &= !libc::ECHO;
        unsafe { libc::tcsetattr(fd, libc::TCSANOW, &quiet) };
    }
    let mut line = String::new();
    let _ = std::io::stdin().lock().read_line(&mut line);
    if tty {
        unsafe { libc::tcsetattr(fd, libc::TCSANOW, &old) };
        eprintln!();
    }
    line.trim_end_matches(['\n', '\r']).to_string()
}

async fn serve() -> i32 {
    util::log_open();
    let cfg = Config::load();
    say!(
        "s24-remote {VERSION}: starting (port {}, tunnel {}, helper {})",
        cfg.port,
        match cfg.tunnel {
            Tunnel::Quick => "quick",
            Tunnel::Token { .. } => "named",
            Tunnel::None => "none",
        },
        cfg.helper.display()
    );
    if std::env::var_os("WAYLAND_DISPLAY").is_none() {
        say!("no desktop session here (WAYLAND_DISPLAY isn't set)");
        return 1;
    }
    let auth = Auth::new();
    let status = Arc::new(watch::channel(Status::new(auth.code())).0);
    let (ask_tx, mut asks) = mpsc::unbounded_channel();
    let bus = match dbus::Bus::start(status.subscribe(), ask_tx.clone()).await {
        Ok(b) => b,
        Err(e) => {
            say!("{e}");
            return 1;
        }
    };
    let _ = std::fs::create_dir_all(util::runtime_dir());
    let _ = std::fs::write(pid_file(), std::process::id().to_string());

    let listener = match tokio::net::TcpListener::bind(("127.0.0.1", cfg.port)).await {
        Ok(l) => l,
        Err(e) => {
            say!("can't listen on 127.0.0.1:{}: {e}", cfg.port);
            bus.notify(0, "Remote Desktop couldn't start", &format!("Port {} is in use: {e}", cfg.port), &[], false).await;
            let _ = std::fs::remove_file(pid_file());
            return 1;
        }
    };

    let (ev_tx, ev_rx) = mpsc::unbounded_channel();
    let core = core::Core::new(cfg.clone(), bus.conn.clone(), ev_tx.clone(), status.clone());
    tokio::spawn(core.run(ev_rx));
    let app = Arc::new(http::App {
        port: cfg.port,
        auth: Mutex::new(auth),
        login_turn: tokio::sync::Mutex::new(()),
        status: status.clone(),
        core: ev_tx.clone(),
        next_id: AtomicU64::new(0),
        hostname: cfg.hostname.clone(),
        machine: lan::machine_name(),
        asks: ask_tx.clone(),
    });
    tokio::spawn(http::serve(app.clone(), listener));
    // The Wi-Fi door: the same sign-in on the phone's own address, for a computer on this network.
    if cfg.lan {
        if let Err(e) = lan::start(app.clone(), cfg.lan_port).await {
            say!("wi-fi: not open: {e}");
        }
    }

    // Awake while on: asleep, the phone is off the internet and nothing a browser sends wakes it.
    let sleep_lock = dbus::inhibit_sleep().await;
    if sleep_lock.is_none() {
        say!("couldn't keep the phone awake: it may sleep and drop the tunnel");
    }

    let (way_tx, mut way) = watch::channel(Way::Starting);
    tokio::spawn(tunnel::run(cfg.clone(), way_tx));

    let mut term = signal(SignalKind::terminate()).expect("SIGTERM");
    let mut int = signal(SignalKind::interrupt()).expect("SIGINT");
    let mut usr1 = signal(SignalKind::user_defined1()).expect("SIGUSR1");
    let mut st = status.subscribe();
    let mut note = 0u32;
    let mut shown = (String::new(), String::new());
    let mut last_viewer: Option<String> = None;
    let _ = util::write_atomic(&status_file(), &status.borrow().to_json());
    loop {
        tokio::select! {
            r = way.changed() => {
                if r.is_err() { continue; }
                let w = way.borrow().clone();
                let quick = !matches!(cfg.tunnel, Tunnel::Token { .. });
                status.send_modify(|s| match w {
                    Way::Starting => {
                        // A quick tunnel that starts again gets a new address: the old one is dead.
                        if quick {
                            s.url = None;
                        }
                        if s.state != State::Connected {
                            s.state = State::Starting;
                        }
                    }
                    Way::Found(url) => s.trying = Some(url),
                    Way::Ready(url) => {
                        s.url = Some(url);
                        s.trying = None;
                        s.message.clear();
                        if s.state != State::Connected { s.state = State::Ready }
                    }
                    Way::Down(msg) => {
                        s.message = msg;
                        if s.state != State::Connected { s.state = State::Error }
                    }
                });
            }
            r = st.changed() => {
                if r.is_err() { break; }
                let s = st.borrow_and_update().clone();
                let _ = util::write_atomic(&status_file(), &s.to_json());
                let text = note_text(&s);
                if text != shown {
                    shown = text.clone();
                    let actions: &[&str] = if s.url.is_some() && s.state != State::Error {
                        &["copy", "Copy link", "off", "Turn off"]
                    } else {
                        &["off", "Turn off"]
                    };
                    note = bus.notify(note, &text.0, &text.1, actions, true).await;
                    dbus::OUR_NOTE.store(note, std::sync::atomic::Ordering::Relaxed);
                }
                if s.viewer != last_viewer {
                    if let Some(w) = &s.viewer {
                        bus.notify(0, "A browser opened your desktop", &format!("From {w}. Turn Remote Desktop off to close it."), &[], false).await;
                    }
                    last_viewer = s.viewer.clone();
                }
            }
            ask = asks.recv() => match ask {
                None => break,
                Some(dbus::Ask::Off) => {
                    // Turn off, in the notification: off for good, not only until the next restart
                    keep_on(false);
                    break;
                }
                Some(dbus::Ask::Note { title, body }) => {
                    bus.notify(0, &title, &body, &[], false).await;
                }
                Some(dbus::Ask::Show) => {
                    let s = status.borrow().clone();
                    let text = note_text(&s);
                    note = bus.notify(0, &text.0, &text.1, &["copy", "Copy link", "off", "Turn off"], true).await;
                    dbus::OUR_NOTE.store(note, std::sync::atomic::Ordering::Relaxed);
                }
                Some(dbus::Ask::Copy) => {
                    let s = status.borrow().clone();
                    if let Some(url) = s.url {
                        copy_to_clipboard(&cfg, &format!("{url}/#c={}", s.code));
                    }
                }
            },
            _ = term.recv() => break,
            _ = int.recv() => break,
            _ = usr1.recv() => { let _ = ask_tx.send(dbus::Ask::Show); }
        }
    }
    say!("turning off");
    let (done_tx, done_rx) = oneshot::channel();
    if ev_tx.send(core::Ev::Stop(done_tx)).is_ok() {
        let _ = tokio::time::timeout(Duration::from_secs(12), done_rx).await;
    }
    bus.close_note(note).await;
    drop(sleep_lock);
    let _ = std::fs::remove_file(status_file());
    let _ = std::fs::remove_file(pid_file());
    say!("off");
    0
}

/// The phone's clipboard, through the helper's `clip --set` (it holds the text for 10 minutes).
fn copy_to_clipboard(cfg: &Config, text: &str) {
    let r = tokio::process::Command::new(&cfg.helper)
        .args(["clip", "--set", text])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();
    match r {
        Ok(mut child) => {
            say!("the link is on the phone's clipboard");
            tokio::spawn(async move {
                let _ = child.wait().await;
            });
        }
        Err(e) => say!("couldn't copy the link: {e}"),
    }
}
