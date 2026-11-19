//! The session bus: our name (one copy at a time, and what the quick setting watches), the
//! notification with the address and code, and logind's sleep lock.

use std::collections::HashMap;

use futures_util::StreamExt;
use tokio::sync::{mpsc, watch};
use zbus::zvariant::{OwnedFd, Value};

use crate::status::Status;
use crate::say;

pub const NAME: &str = "org.s24.Remote";
pub const PATH: &str = "/org/s24/Remote";

/// What the bus asks of the daemon.
#[derive(Debug)]
pub enum Ask {
    Off,
    Show,
    Copy,
}

pub struct Remote {
    status: watch::Receiver<Status>,
    asks: mpsc::UnboundedSender<Ask>,
}

#[zbus::interface(name = "org.s24.Remote")]
impl Remote {
    /// "off", "starting", "ready", "connected" or "error".
    #[zbus(property)]
    fn state(&self) -> String {
        self.status.borrow().state.to_string()
    }

    /// One line for the quick setting.
    #[zbus(property)]
    fn summary(&self) -> String {
        self.status.borrow().summary()
    }

    /// Everything, as JSON.
    fn status(&self) -> String {
        self.status.borrow().to_json()
    }

    fn off(&self) {
        let _ = self.asks.send(Ask::Off);
    }

    /// The notification with the address and code, again.
    fn show(&self) {
        let _ = self.asks.send(Ask::Show);
    }
}

pub struct Bus {
    pub conn: zbus::Connection,
}

impl Bus {
    /// Our name on the session bus. Err when another copy has it: then this one isn't needed.
    pub async fn start(status: watch::Receiver<Status>, asks: mpsc::UnboundedSender<Ask>) -> Result<Bus, String> {
        let conn = zbus::connection::Builder::session()
            .map_err(|e| e.to_string())?
            .serve_at(PATH, Remote { status: status.clone(), asks: asks.clone() })
            .map_err(|e| e.to_string())?
            .build()
            .await
            .map_err(|e| format!("session bus: {e}"))?;
        let flags = zbus::fdo::RequestNameFlags::DoNotQueue.into();
        match conn.request_name_with_flags(NAME, flags).await {
            Ok(zbus::fdo::RequestNameReply::PrimaryOwner) => {}
            Ok(r) => return Err(format!("{NAME} is taken ({r:?}): Remote Desktop is on already")),
            Err(e) => return Err(format!("{NAME}: {e}")),
        }
        // The quick setting shows the summary: tell it when it changes.
        let c = conn.clone();
        let mut rx = status;
        tokio::spawn(async move {
            let mut last = (String::new(), String::new());
            while rx.changed().await.is_ok() {
                let now = {
                    let s = rx.borrow();
                    (s.state.to_string(), s.summary())
                };
                if now == last {
                    continue;
                }
                last = now;
                if let Ok(iface) = c.object_server().interface::<_, Remote>(PATH).await {
                    let ctx = iface.signal_emitter();
                    let r = iface.get().await;
                    let _ = r.state_changed(ctx).await;
                    let _ = r.summary_changed(ctx).await;
                }
            }
        });
        // Our notification's buttons.
        let c = conn.clone();
        tokio::spawn(async move {
            let rule = match zbus::MatchRule::builder()
                .msg_type(zbus::message::Type::Signal)
                .interface("org.freedesktop.Notifications")
                .and_then(|b| b.member("ActionInvoked"))
            {
                Ok(b) => b.build(),
                Err(_) => return,
            };
            let Ok(mut stream) = zbus::MessageStream::for_match_rule(rule, &c, None).await else { return };
            while let Some(Ok(msg)) = stream.next().await {
                if let Ok((id, key)) = msg.body().deserialize::<(u32, String)>() {
                    if id != 0 && id == OUR_NOTE.load(std::sync::atomic::Ordering::Relaxed) {
                        let _ = asks.send(match key.as_str() {
                            "off" => Ask::Off,
                            "copy" => Ask::Copy,
                            _ => Ask::Show,
                        });
                    }
                }
            }
        });
        Ok(Bus { conn })
    }

    /// Show (or replace) the notification. Returns its id.
    pub async fn notify(&self, replaces: u32, summary: &str, body: &str, actions: &[&str], resident: bool) -> u32 {
        let mut hints: HashMap<&str, Value> = HashMap::new();
        hints.insert("urgency", Value::U8(1));
        hints.insert("desktop-entry", Value::from("org.s24.remote"));
        if resident {
            hints.insert("resident", Value::Bool(true));
        }
        let r = self
            .conn
            .call_method(
                Some("org.freedesktop.Notifications"),
                "/org/freedesktop/Notifications",
                Some("org.freedesktop.Notifications"),
                "Notify",
                &("Remote Desktop", replaces, "krfb", summary, body, actions.to_vec(), hints, 0i32),
            )
            .await;
        match r {
            Ok(m) => m.body().deserialize::<u32>().unwrap_or(0),
            Err(e) => {
                say!("notify: {e}");
                0
            }
        }
    }

    pub async fn close_note(&self, id: u32) {
        if id == 0 {
            return;
        }
        let _ = self
            .conn
            .call_method(
                Some("org.freedesktop.Notifications"),
                "/org/freedesktop/Notifications",
                Some("org.freedesktop.Notifications"),
                "CloseNotification",
                &(id,),
            )
            .await;
    }
}

/// The notification whose buttons we answer.
pub static OUR_NOTE: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

/// logind's "don't sleep" while Remote Desktop is on: a sleeping phone drops off the internet, and
/// nothing a browser sends wakes it. (s24-powerd honours logind's block inhibitors.)
pub async fn inhibit_sleep() -> Option<OwnedFd> {
    let sys = zbus::Connection::system().await.ok()?;
    let r = sys
        .call_method(
            Some("org.freedesktop.login1"),
            "/org/freedesktop/login1",
            Some("org.freedesktop.login1.Manager"),
            "Inhibit",
            &("sleep", "Remote Desktop", "The desktop is open to browsers", "block"),
        )
        .await;
    match r {
        Ok(m) => m.body().deserialize::<OwnedFd>().ok(),
        Err(e) => {
            say!("sleep lock: {e}");
            None
        }
    }
}
