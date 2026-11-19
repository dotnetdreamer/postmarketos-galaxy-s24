//! s24-remote: the phone's desktop in any browser, from anywhere.
//!
//!   s24-remote on | off | toggle     what the Remote Desktop quick setting runs
//!   s24-remote status [--json]       on or off, the address and the code
//!   s24-remote show                  the notification with the address and code, again
//!   s24-remote serve                 the program itself (what `on` starts, in the background)
//!   s24-remote cleanup               at login: the phone's screen back if a run left it off
//!
//! While it is on: a tunnel to the internet (tunnel.rs), a door on 127.0.0.1 that only opens to
//! the code shown on the phone (http.rs, auth.rs), and once a browser is in, a screen of its own
//! on the phone's desktop in docked mode, encoded as H.264 by Open Android DeX's postmarketOS
//! helper and paced to the browser's link (core.rs, flow.rs). The phone stays awake while it is
//! on (logind), and its own screen is off while a browser is connected (phone.rs).

mod auth;
mod config;
mod core;
mod dbus;
mod flow;
mod h264;
mod helper;
mod http;
mod phone;
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
        "on" => on(),
        "off" => off(),
        "toggle" => {
            if running_pid().is_some() {
                off()
            } else {
                on()
            }
        }
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
            eprintln!("usage: s24-remote on | off | toggle | status [--json] | show | serve | cleanup | version");
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
    match (&s.state, &s.url) {
        (State::Error, _) => ("Remote Desktop can't be reached".into(), s.message.clone()),
        (_, None) => ("Remote Desktop is starting".into(), "Getting an address on the internet…".into()),
        (_, Some(url)) if s.locked_secs > 0 => (
            "Remote Desktop: sign-in closed".into(),
            format!("Too many wrong codes were typed at {url}. Turn Remote Desktop off and on to open it again."),
        ),
        (_, Some(url)) => (
            "Remote Desktop is on".into(),
            format!("Open {url} in any browser and type the code {}", spaced(&s.code)),
        ),
    }
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
    });
    tokio::spawn(http::serve(app.clone(), listener));

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
                Some(dbus::Ask::Off) | None => break,
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
