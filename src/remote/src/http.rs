//! The door: the page, the sign-in, and the WebSocket the desktop travels on.
//!
//! Two ways in, the same door behind both: 127.0.0.1, which only the tunnel reaches from outside,
//! and the Wi-Fi door (lan.rs, HTTPS on the phone's own address). Every request must name an
//! address of its way in, in Host (the tunnel's, or this phone's own address on that network),
//! which keeps another site's page from reaching it through a name of its own (DNS rebinding);
//! everything that changes something, and the WebSocket, must come from this origin's own page
//! (Origin); the cookie is HttpOnly, Secure and SameSite=Strict, and the page may load nothing from
//! anywhere else (CSP).

use std::convert::Infallible;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use http_body_util::{BodyExt, Full, Limited};
use hyper::body::Incoming;
use hyper::header::{self, HeaderValue};
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use serde_json::{json, Value};
use tokio::net::TcpListener;
use tokio::sync::{mpsc, watch};
use tokio_tungstenite::tungstenite::protocol::{Role, WebSocketConfig};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;

use crate::auth::{Auth, Login};
use crate::core::{Ev, Out};
use crate::dbus::Ask;
use crate::helper::{IN_ACK, IN_AWAKE, IN_KEY, IN_POINTER};
use crate::say;
use crate::status::Status;
use crate::www;

pub struct App {
    pub port: u16,
    pub auth: Mutex<Auth>,
    /// Sign-in tries one at a time, each wrong one a second long.
    pub login_turn: tokio::sync::Mutex<()>,
    pub status: Arc<watch::Sender<Status>>,
    pub core: mpsc::UnboundedSender<Ev>,
    pub next_id: AtomicU64,
    /// The named tunnel's address, when there is one (the quick tunnel's is in status).
    pub hostname: Option<String>,
    /// This phone's name (galaxy-s24), for the Wi-Fi door's Host check.
    pub machine: String,
    /// Notifications on the phone (a password changed).
    pub asks: mpsc::UnboundedSender<Ask>,
}

/// Which way a request came in.
#[derive(Clone, Copy, Debug)]
pub enum Door {
    /// 127.0.0.1: the tunnel, or a browser on the phone itself.
    Local { peer: SocketAddr },
    /// The Wi-Fi door (lan.rs), on this address of the phone's.
    Lan { peer: SocketAddr, local: SocketAddr },
}

type Resp = Response<Full<Bytes>>;

const COOKIE: &str = "s24r";
const CSP: &str = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; \
                   connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'";

fn respond(status: StatusCode, ctype: &str, body: impl Into<Bytes>) -> Resp {
    let mut r = Response::new(Full::new(body.into()));
    *r.status_mut() = status;
    let h = r.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_str(ctype).unwrap());
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    h.insert("content-security-policy", HeaderValue::from_static(CSP));
    h.insert("x-content-type-options", HeaderValue::from_static("nosniff"));
    h.insert("referrer-policy", HeaderValue::from_static("no-referrer"));
    h.insert("x-frame-options", HeaderValue::from_static("DENY"));
    h.insert("cross-origin-opener-policy", HeaderValue::from_static("same-origin"));
    r
}

fn json_resp(status: StatusCode, v: Value) -> Resp {
    respond(status, "application/json", v.to_string())
}

fn header_str<'a>(req: &'a Request<Incoming>, name: &str) -> &'a str {
    req.headers().get(name).and_then(|v| v.to_str().ok()).unwrap_or("")
}

fn cookie_token(req: &Request<Incoming>) -> Option<String> {
    for v in req.headers().get_all(header::COOKIE) {
        let Ok(s) = v.to_str() else { continue };
        for part in s.split(';') {
            if let Some((k, val)) = part.trim().split_once('=') {
                if k == COOKIE && val.len() == 64 && val.bytes().all(|b| b.is_ascii_hexdigit()) {
                    return Some(val.to_string());
                }
            }
        }
    }
    None
}

/// Where a browser came from: as Cloudflare saw it, or on this Wi-Fi. (Cloudflare's headers count
/// only from the tunnel: on the Wi-Fi anyone could send them.)
fn who(req: &Request<Incoming>, door: Door) -> String {
    let peer = match door {
        Door::Lan { peer, .. } => return format!("{} on this Wi-Fi", crate::lan::canonical(peer.ip())),
        Door::Local { peer } => peer,
    };
    let ip = header_str(req, "cf-connecting-ip");
    let country = header_str(req, "cf-ipcountry");
    let ip: String = if ip.is_empty() { peer.ip().to_string() } else { ip.chars().filter(|c| c.is_ascii_hexdigit() || ".:".contains(*c)).take(45).collect() };
    let country: String = country.chars().filter(|c| c.is_ascii_alphanumeric()).take(4).collect();
    if country.is_empty() || country == "XX" {
        ip
    } else {
        format!("{ip}, {country}")
    }
}

impl App {
    /// The Host header is one of ours, for the way the request came in.
    fn host_ok(&self, host: &str, door: Door) -> bool {
        if let Door::Lan { local, .. } = door {
            return crate::lan::host_is_ours(host, local, &self.machine);
        }
        let host = host.to_ascii_lowercase();
        let local = [format!("127.0.0.1:{}", self.port), format!("localhost:{}", self.port)];
        if local.contains(&host) {
            return true;
        }
        if self.hostname.as_deref() == Some(host.as_str()) {
            return true;
        }
        self.status.borrow().is_our_host(&host)
    }

    /// The request comes from our own page.
    fn origin_ok(&self, req: &Request<Incoming>) -> bool {
        let origin = header_str(req, "origin").to_ascii_lowercase();
        let host = header_str(req, "host").to_ascii_lowercase();
        let https = origin == format!("https://{host}");
        // Plain http only for this phone's own address (tests, and a browser on the phone).
        let local = origin == format!("http://{host}") && (host.starts_with("127.0.0.1:") || host.starts_with("localhost:"));
        https || local
    }

    fn signed_in(&self, req: &Request<Incoming>) -> Option<String> {
        let t = cookie_token(req)?;
        self.auth.lock().unwrap().check(&t).map(|w| w.to_string())
    }
}

/// The 127.0.0.1 door (the tunnel's).
pub async fn serve(app: Arc<App>, listener: TcpListener) {
    loop {
        let (stream, peer) = match listener.accept().await {
            Ok(x) => x,
            Err(e) => {
                say!("http: accept: {e}");
                tokio::time::sleep(Duration::from_millis(200)).await;
                continue;
            }
        };
        let _ = stream.set_nodelay(true);
        let app = app.clone();
        tokio::spawn(serve_conn(app, stream, Door::Local { peer }));
    }
}

/// One connection, either door's (the Wi-Fi door's after TLS).
pub async fn serve_conn<S>(app: Arc<App>, stream: S, door: Door)
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
    let svc = service_fn(move |req| {
        let app = app.clone();
        async move { Ok::<_, Infallible>(route(app, req, door).await) }
    });
    let conn = hyper::server::conn::http1::Builder::new()
        .timer(hyper_util::rt::TokioTimer::new())
        .keep_alive(true)
        .header_read_timeout(Duration::from_secs(20))
        .serve_connection(TokioIo::new(stream), svc)
        .with_upgrades();
    let _ = conn.await;
}

async fn route(app: Arc<App>, req: Request<Incoming>, door: Door) -> Resp {
    let host = header_str(&req, "host").to_string();
    if !app.host_ok(&host, door) {
        return respond(StatusCode::MISDIRECTED_REQUEST, "text/plain", "Not this address");
    }
    let path = req.uri().path().to_string();
    match (req.method(), path.as_str()) {
        (&Method::GET, "/api/ping") => respond(StatusCode::OK, "text/plain", "ok"),
        (&Method::GET, "/api/session") => match app.signed_in(&req) {
            Some(_) => {
                let (url, lan) = {
                    let s = app.status.borrow();
                    (s.url.clone(), s.lan_url.clone())
                };
                json_resp(StatusCode::OK, json!({"ok": true, "password": crate::password::is_set(), "url": url, "lan": lan}))
            }
            None => {
                let locked = app.auth.lock().unwrap().locked_for(Instant::now()).map(|d| d.as_secs()).unwrap_or(0);
                json_resp(StatusCode::UNAUTHORIZED, json!({"ok": false, "locked": locked}))
            }
        },
        (&Method::POST, "/api/login") => login(app, req, door).await,
        (&Method::POST, "/api/password") => set_password(app, req, door).await,
        (&Method::POST, "/api/logout") => {
            if !app.origin_ok(&req) {
                return respond(StatusCode::FORBIDDEN, "text/plain", "Not from this page");
            }
            if let Some(t) = cookie_token(&req) {
                app.auth.lock().unwrap().logout(&t);
            }
            let mut r = json_resp(StatusCode::OK, json!({"ok": true}));
            r.headers_mut().insert(
                header::SET_COOKIE,
                HeaderValue::from_str(&format!("{COOKIE}=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict")).unwrap(),
            );
            r
        }
        (&Method::GET, "/ws") => websocket(app, req, door).await,
        (&Method::GET, p) => match www::file(p) {
            Some((ctype, body)) => respond(StatusCode::OK, ctype, Bytes::from_static(body)),
            None => respond(StatusCode::NOT_FOUND, "text/plain", "Not found"),
        },
        _ => respond(StatusCode::METHOD_NOT_ALLOWED, "text/plain", "No"),
    }
}

async fn login(app: Arc<App>, req: Request<Incoming>, door: Door) -> Resp {
    if !app.origin_ok(&req) {
        return respond(StatusCode::FORBIDDEN, "text/plain", "Not from this page");
    }
    let from = who(&req, door);
    let body = match Limited::new(req.into_body(), 1024).collect().await {
        Ok(b) => b.to_bytes(),
        Err(_) => return respond(StatusCode::PAYLOAD_TOO_LARGE, "text/plain", "Too long"),
    };
    let code = serde_json::from_slice::<Value>(&body)
        .ok()
        .and_then(|v| v["code"].as_str().map(|s| s.to_string()))
        .unwrap_or_default();
    // One try at a time, from everywhere at once.
    let _turn = app.login_turn.lock().await;
    // The password's hash takes a fifth of a second: worked out on another thread, so the picture
    // (this runtime's one thread) doesn't stop for it, and only when what was typed isn't the code.
    let is_code = app.auth.lock().unwrap().is_code(&code);
    let password_ok = if is_code || code.is_empty() {
        false
    } else {
        let typed = code.clone();
        tokio::task::spawn_blocking(move || crate::password::verify(&typed)).await.unwrap_or(false)
    };
    let result = app.auth.lock().unwrap().login(&code, password_ok, &from, Instant::now());
    match result {
        Login::Ok { token, password } => {
            say!("sign-in: a browser from {from}{}", if password { ", with the password" } else { "" });
            let new_code = app.auth.lock().unwrap().code().to_string();
            app.status.send_modify(|s| s.code = new_code);
            let mut r = json_resp(StatusCode::OK, json!({"ok": true}));
            r.headers_mut().insert(
                header::SET_COOKIE,
                HeaderValue::from_str(&format!("{COOKIE}={token}; Path=/; HttpOnly; Secure; SameSite=Strict")).unwrap(),
            );
            r
        }
        Login::Wrong { left, replaced } => {
            say!("sign-in: a wrong code from {from}{}", if replaced { " (code replaced)" } else { "" });
            if replaced {
                let new_code = app.auth.lock().unwrap().code().to_string();
                app.status.send_modify(|s| {
                    s.code = new_code;
                    s.message = "Too many wrong codes: the code changed".into();
                });
            }
            tokio::time::sleep(Duration::from_secs(1)).await;
            json_resp(StatusCode::UNAUTHORIZED, json!({"ok": false, "left": left, "replaced": replaced}))
        }
        Login::Locked { secs } => {
            say!("sign-in: refused from {from}, locked for {secs} s");
            app.status.send_modify(|s| s.locked_secs = secs);
            tokio::time::sleep(Duration::from_secs(1)).await;
            json_resp(StatusCode::TOO_MANY_REQUESTS, json!({"ok": false, "locked": secs}))
        }
    }
}

/// The owner's password (password.rs), set or taken away from a signed-in browser. The phone is told.
async fn set_password(app: Arc<App>, req: Request<Incoming>, door: Door) -> Resp {
    if !app.origin_ok(&req) {
        return respond(StatusCode::FORBIDDEN, "text/plain", "Not from this page");
    }
    if app.signed_in(&req).is_none() {
        return json_resp(StatusCode::UNAUTHORIZED, json!({"ok": false, "error": "Sign in first"}));
    }
    let from = who(&req, door);
    let body = match Limited::new(req.into_body(), 1024).collect().await {
        Ok(b) => b.to_bytes(),
        Err(_) => return respond(StatusCode::PAYLOAD_TOO_LARGE, "text/plain", "Too long"),
    };
    let v = serde_json::from_slice::<Value>(&body).unwrap_or_default();
    let (result, what) = if v["remove"].as_bool() == Some(true) {
        (crate::password::remove(), "taken away")
    } else {
        // hashing takes a moment: not on the thread that carries the picture
        let pw = v["password"].as_str().unwrap_or("").to_string();
        let r = tokio::task::spawn_blocking(move || crate::password::set(&pw)).await;
        (r.unwrap_or_else(|e| Err(e.to_string())), "set")
    };
    match result {
        Ok(()) => {
            say!("password {what}, from a browser {from}");
            let _ = app.asks.send(Ask::Note {
                title: format!("Remote Desktop's password was {what}"),
                body: format!("From a browser {from}. If that wasn't you, turn Remote Desktop off."),
            });
            json_resp(StatusCode::OK, json!({"ok": true, "password": crate::password::is_set()}))
        }
        Err(e) => json_resp(StatusCode::BAD_REQUEST, json!({"ok": false, "error": e})),
    }
}

async fn websocket(app: Arc<App>, mut req: Request<Incoming>, door: Door) -> Resp {
    if !app.origin_ok(&req) {
        return respond(StatusCode::FORBIDDEN, "text/plain", "Not from this page");
    }
    let Some(_) = app.signed_in(&req) else {
        return respond(StatusCode::UNAUTHORIZED, "text/plain", "Sign in first");
    };
    let upgrade = header_str(&req, "upgrade").eq_ignore_ascii_case("websocket");
    let key = header_str(&req, "sec-websocket-key").to_string();
    if !upgrade || key.is_empty() {
        return respond(StatusCode::BAD_REQUEST, "text/plain", "WebSocket only");
    }
    let from = who(&req, door);
    let lan = matches!(door, Door::Lan { .. });
    let accept = tokio_tungstenite::tungstenite::handshake::derive_accept_key(key.as_bytes());
    let on_upgrade = hyper::upgrade::on(&mut req);
    let id = app.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    let core = app.core.clone();
    tokio::spawn(async move {
        match on_upgrade.await {
            Ok(up) => {
                let cfg = WebSocketConfig::default().max_message_size(Some(2 << 20)).max_frame_size(Some(2 << 20));
                let ws = WebSocketStream::from_raw_socket(TokioIo::new(up), Role::Server, Some(cfg)).await;
                viewer(ws, id, from, lan, core).await;
            }
            Err(e) => say!("websocket: upgrade failed: {e}"),
        }
    });
    let mut r = Response::new(Full::new(Bytes::new()));
    *r.status_mut() = StatusCode::SWITCHING_PROTOCOLS;
    let h = r.headers_mut();
    h.insert(header::UPGRADE, HeaderValue::from_static("websocket"));
    h.insert(header::CONNECTION, HeaderValue::from_static("Upgrade"));
    h.insert("sec-websocket-accept", HeaderValue::from_str(&accept).unwrap());
    r
}

/// One browser's WebSocket, between it and the core.
async fn viewer<S>(ws: WebSocketStream<S>, id: u64, from: String, lan: bool, core: mpsc::UnboundedSender<Ev>)
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin,
{
    let (mut sink, mut stream) = ws.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<Out>();
    let mut tx = Some(tx);
    let mut opened = false;
    let hello_by = tokio::time::Instant::now() + Duration::from_secs(15);
    let mut last_heard = Instant::now();
    let mut check = tokio::time::interval(Duration::from_secs(5));
    loop {
        tokio::select! {
            msg = stream.next() => {
                let Some(Ok(msg)) = msg else { break };
                last_heard = Instant::now();
                match msg {
                    Message::Text(t) => {
                        let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                        if !opened {
                            if v["t"] == "hello" {
                                opened = true;
                                let _ = core.send(Ev::Open { id, tx: tx.take().unwrap(), hello: v, who: from.clone(), lan });
                            }
                            continue;
                        }
                        let _ = core.send(Ev::Text { id, v });
                    }
                    Message::Binary(b) if opened && !b.is_empty() => match b[0] {
                        IN_ACK if b.len() == 5 => {
                            let seq = u32::from_be_bytes([b[1], b[2], b[3], b[4]]);
                            let _ = core.send(Ev::Ack { id, seq });
                        }
                        IN_POINTER | IN_KEY => {
                            let _ = core.send(Ev::Input { id, data: b.to_vec() });
                        }
                        // It did its job by arriving: the Wi-Fi is awake.
                        IN_AWAKE => {}
                        _ => {}
                    },
                    Message::Ping(p) => {
                        let _ = sink.send(Message::Pong(p)).await;
                    }
                    Message::Close(_) => break,
                    _ => {}
                }
            }
            out = rx.recv() => {
                let Some(out) = out else { break };
                let r = match out {
                    Out::Text(t) => sink.send(Message::Text(t.into())).await,
                    Out::Bin(b) => sink.send(Message::Binary(b)).await,
                    Out::Close => {
                        let _ = sink.close().await;
                        break;
                    }
                };
                if r.is_err() {
                    break;
                }
            }
            _ = tokio::time::sleep_until(hello_by), if !opened => break,
            _ = check.tick() => {
                // The page pings every 2 s (a hidden tab maybe once a minute); 75 s of nothing and it is gone.
                if last_heard.elapsed() > Duration::from_secs(75) {
                    say!("browser {from}: silent for 75 s, letting it go");
                    break;
                }
            }
        }
    }
    if opened {
        let _ = core.send(Ev::Closed { id });
    }
}
