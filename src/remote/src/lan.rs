//! The Wi-Fi door: the same page and sign-in on the phone's own address on the local network, for a
//! computer on the same Wi-Fi (or on the USB cable's network). Nothing goes out to the internet.
//!
//! - HTTPS, because browsers give video decoding (WebCodecs) to secure pages only. Nobody can vouch
//!   for a phone's own address, so the certificate is the phone's own: a browser warns about it the
//!   first time, and after "continue" it is a secure page like any other
//! - The key is made once and kept (~/.local/share/s24-remote/wifi-key.pem, 0600). The certificate
//!   names the phone's name (and name.local) and its IPv4 addresses on Wi-Fi, USB and wired networks;
//!   it is made again when those change, and a month before its 800 days run out (Apple's devices
//!   refuse server certificates longer than 825 days). Each new one has a new random serial number:
//!   Firefox refuses a certificate whose issuer and serial it saw on a different one before
//! - Only for the local network, checked here: the Galaxy S24's kernel has no nftables, so the phone
//!   has no firewall to lean on (rootfs/etc/nftables.d/50_s24remote.nft is for phones that do). It
//!   listens on IPv4 only (on mobile data a phone can have an IPv6 address the whole internet
//!   reaches), and takes a connection only if it came to the phone's address on Wi-Fi, USB or a wired
//!   network, from a private address (10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16)
//! - The same door as the tunnel's (http.rs) behind it: the code or password, the cookie, Host and
//!   Origin checks (Host must be this phone's own address or name), one browser at a time

use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use std::time::Duration;

use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use rustls::ServerConfig;
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;

use crate::http::{App, Door};
use crate::say;
use crate::util::{data_dir, random_bytes, write_private};

/// At most this many connections through the Wi-Fi door at once (anyone on the Wi-Fi can knock).
const MAX_CONNS: usize = 64;

/// Wi-Fi, the USB cable's network, wired networks (a USB-C hub's Ethernet): the same names the
/// firewall rule opens the port on.
pub fn lan_interface(name: &str) -> bool {
    ["wlan", "usb", "eth", "en"].iter().any(|p| name.starts_with(p))
}

/// The phone's IPv4 addresses on those networks, Wi-Fi's first.
pub fn addresses() -> Vec<Ipv4Addr> {
    let mut found: Vec<(u8, Ipv4Addr)> = vec![];
    unsafe {
        let mut ifap: *mut libc::ifaddrs = std::ptr::null_mut();
        if libc::getifaddrs(&mut ifap) != 0 {
            return vec![];
        }
        let mut p = ifap;
        while !p.is_null() {
            let ifa = &*p;
            p = ifa.ifa_next;
            if ifa.ifa_addr.is_null() || (*ifa.ifa_addr).sa_family as i32 != libc::AF_INET {
                continue;
            }
            if ifa.ifa_flags & (libc::IFF_UP as u32) == 0 {
                continue;
            }
            let name = std::ffi::CStr::from_ptr(ifa.ifa_name).to_string_lossy().to_string();
            if !lan_interface(&name) {
                continue;
            }
            let sin = &*(ifa.ifa_addr as *const libc::sockaddr_in);
            let ip = Ipv4Addr::from(u32::from_be(sin.sin_addr.s_addr));
            if ip.is_loopback() || ip.is_link_local() || ip.is_unspecified() {
                continue;
            }
            let rank = if name.starts_with("wlan") { 0 } else if name.starts_with("usb") { 2 } else { 1 };
            if !found.iter().any(|(_, a)| *a == ip) {
                found.push((rank, ip));
            }
        }
        libc::freeifaddrs(ifap);
    }
    found.sort();
    found.into_iter().map(|(_, ip)| ip).collect()
}

/// This phone's name (galaxy-s24), lower case; empty if it has none worth using.
pub fn machine_name() -> String {
    let mut buf = [0u8; 256];
    let r = unsafe { libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) };
    if r != 0 {
        return String::new();
    }
    let n = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let name = String::from_utf8_lossy(&buf[..n]).trim().to_ascii_lowercase();
    let ok = !name.is_empty()
        && name != "localhost"
        && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '.');
    if ok {
        name
    } else {
        String::new()
    }
}

/// The names the certificate is for: the phone's name, name.local, its addresses.
pub fn cert_names(machine: &str, ips: &[Ipv4Addr]) -> Vec<String> {
    let mut v = vec![];
    if !machine.is_empty() {
        v.push(machine.to_string());
        if !machine.ends_with(".local") {
            v.push(format!("{machine}.local"));
        }
    }
    v.extend(ips.iter().map(|i| i.to_string()));
    v
}

/// Whether `host` (a Host header, port included) names this phone through the Wi-Fi door that
/// `local` (the connection's own address on the phone) belongs to.
pub fn host_is_ours(host: &str, local: SocketAddr, machine: &str) -> bool {
    let host = host.to_ascii_lowercase();
    let port = local.port();
    let ip = canonical(local.ip());
    let by_ip = match ip {
        IpAddr::V4(a) => format!("{a}:{port}"),
        IpAddr::V6(a) => format!("[{a}]:{port}"),
    };
    if host == by_ip {
        return true;
    }
    !machine.is_empty() && (host == format!("{machine}:{port}") || host == format!("{machine}.local:{port}"))
}

/// An IPv4 address that came in on the dual-stack socket as ::ffff:a.b.c.d, as itself.
pub fn canonical(ip: IpAddr) -> IpAddr {
    match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map(IpAddr::V4).unwrap_or(IpAddr::V6(v6)),
        v4 => v4,
    }
}

fn key_file() -> PathBuf {
    data_dir().join("wifi-key.pem")
}
fn cert_file() -> PathBuf {
    data_dir().join("wifi-cert.der")
}
fn names_file() -> PathBuf {
    data_dir().join("wifi-cert.names")
}

/// The kept key, or a new one (kept from now on).
fn key() -> Result<rcgen::KeyPair, String> {
    if let Some(k) = std::fs::read_to_string(key_file()).ok().and_then(|p| rcgen::KeyPair::from_pem(&p).ok()) {
        return Ok(k);
    }
    let k = rcgen::KeyPair::generate().map_err(|e| format!("a key for the Wi-Fi certificate: {e}"))?;
    write_private(&key_file(), k.serialize_pem().as_bytes()).map_err(|e| format!("{}: {e}", key_file().display()))?;
    Ok(k)
}

/// At most this many addresses in a certificate: the current ones first, then the latest kept ones.
const MAX_CERT_IPS: usize = 8;

/// The names a new certificate gets: the ones needed now, and the kept certificate's too (up to
/// MAX_CERT_IPS addresses), so that going back to a network it knew doesn't need a new one.
pub fn merged_names(now: &[String], kept: &[String]) -> Vec<String> {
    let is_ip = |n: &String| n.parse::<IpAddr>().is_ok();
    let mut out: Vec<String> = now.iter().filter(|n| !is_ip(n)).cloned().collect();
    let mut ips = 0;
    for n in now.iter().chain(kept.iter()).filter(|n| is_ip(n)) {
        if ips < MAX_CERT_IPS && !out.contains(n) {
            out.push(n.clone());
            ips += 1;
        }
    }
    out
}

/// A certificate for `names`: the kept one if it names all of them and has a month left, else a new
/// one for them and the kept one's names. A browser's "continue" holds as long as the certificate
/// does: at boot the phone's Wi-Fi comes up after Remote Desktop starts, and the certificate must
/// not change on the way.
fn certificate(names: &[String]) -> Result<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>), String> {
    let key = key()?;
    let key_der = || PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.serialize_der()));
    let now = time::OffsetDateTime::now_utc();
    let mut kept: Vec<String> = vec![];
    if let (Ok(der), Ok(meta)) = (std::fs::read(cert_file()), std::fs::read_to_string(names_file())) {
        let mut lines = meta.lines();
        kept = lines.next().unwrap_or("").split_whitespace().map(String::from).collect();
        let until: i64 = lines.next().and_then(|l| l.trim().parse().ok()).unwrap_or(0);
        let covered = names.iter().all(|n| kept.contains(n));
        if covered && until - now.unix_timestamp() > 30 * 86400 && !der.is_empty() {
            return Ok((vec![CertificateDer::from(der)], key_der()));
        }
    }
    let names = merged_names(names, &kept);
    let mut params = rcgen::CertificateParams::new(names.to_vec()).map_err(|e| format!("certificate names: {e}"))?;
    let mut dn = rcgen::DistinguishedName::new();
    let machine = machine_name();
    dn.push(rcgen::DnType::CommonName, if machine.is_empty() { "Phone Desktop".to_string() } else { format!("{machine} Remote Desktop") });
    params.distinguished_name = dn;
    params.not_before = now - time::Duration::days(1);
    params.not_after = now + time::Duration::days(800);
    params.extended_key_usages = vec![rcgen::ExtendedKeyUsagePurpose::ServerAuth];
    params.key_usages = vec![rcgen::KeyUsagePurpose::DigitalSignature];
    let mut serial = random_bytes(16);
    serial[0] &= 0x7f;
    params.serial_number = Some(rcgen::SerialNumber::from_slice(&serial));
    let cert = params.self_signed(&key).map_err(|e| format!("the Wi-Fi certificate: {e}"))?;
    let der = cert.der().to_vec();
    write_private(&cert_file(), &der).map_err(|e| format!("{}: {e}", cert_file().display()))?;
    let meta = format!("{}\n{}\n", names.join(" "), params.not_after.unix_timestamp());
    write_private(&names_file(), meta.as_bytes()).map_err(|e| format!("{}: {e}", names_file().display()))?;
    say!("wi-fi: a new certificate for {}", names.join(", "));
    Ok((vec![CertificateDer::from(der)], key_der()))
}

fn server_config(names: &[String]) -> Result<Arc<ServerConfig>, String> {
    let (chain, key) = certificate(names)?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut cfg = ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| format!("TLS: {e}"))?
        .with_no_client_auth()
        .with_single_cert(chain, key)
        .map_err(|e| format!("TLS: {e}"))?;
    cfg.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(cfg))
}

fn url_for(ips: &[Ipv4Addr], port: u16) -> Option<String> {
    ips.first().map(|ip| if port == 443 { format!("https://{ip}") } else { format!("https://{ip}:{port}") })
}

/// IPv4 only: on mobile data the phone can have an IPv6 address the whole internet reaches.
async fn bind(port: u16) -> std::io::Result<TcpListener> {
    TcpListener::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, port))).await
}

/// A local network's address: the private ranges, the carriers' shared range (some Wi-Fi uses it),
/// link-local.
pub fn private(ip: IpAddr) -> bool {
    match canonical(ip) {
        IpAddr::V4(a) => {
            let o = a.octets();
            a.is_private() || a.is_link_local() || (o[0] == 100 && (o[1] & 0xc0) == 64)
        }
        IpAddr::V6(_) => false,
    }
}

/// Whether a connection may come in: to the phone's address on a local network (`lan`, refreshed by
/// the caller), from a private address.
pub fn admit(local: SocketAddr, peer: SocketAddr, lan: &[Ipv4Addr]) -> bool {
    let to_lan = match canonical(local.ip()) {
        IpAddr::V4(a) => lan.contains(&a),
        IpAddr::V6(_) => false,
    };
    to_lan && private(peer.ip())
}

/// Open the Wi-Fi door. Err (and nothing open) if it can't listen or make its certificate.
pub async fn start(app: Arc<App>, port: u16) -> Result<(), String> {
    let listener = bind(port).await.map_err(|e| format!("port {port}: {e}"))?;
    let machine = machine_name();
    let mut ips = addresses();
    let config = Arc::new(RwLock::new(server_config(&cert_names(&machine, &ips))?));
    let url = url_for(&ips, port);
    say!("wi-fi: open on port {port}{}", url.as_deref().map(|u| format!(", {u}")).unwrap_or_default());
    app.status.send_modify(|s| s.lan_url = url);

    // The addresses change with the network: a certificate for the new ones, and the new address.
    let watched = config.clone();
    let status = app.status.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(20)).await;
            let now = addresses();
            if now == ips {
                continue;
            }
            match server_config(&cert_names(&machine, &now)) {
                Ok(c) => *watched.write().unwrap() = c,
                Err(e) => say!("wi-fi: {e}"),
            }
            let url = url_for(&now, port);
            say!("wi-fi: the phone's address is now {}", url.as_deref().unwrap_or("none (no Wi-Fi)"));
            status.send_modify(|s| s.lan_url = url);
            ips = now;
        }
    });

    let open = Arc::new(AtomicUsize::new(0));
    let lan = Arc::new(RwLock::new(addresses()));
    tokio::spawn(async move {
        loop {
            let (stream, peer) = match listener.accept().await {
                Ok(x) => x,
                Err(e) => {
                    say!("wi-fi: accept: {e}");
                    tokio::time::sleep(Duration::from_millis(200)).await;
                    continue;
                }
            };
            if open.load(Ordering::Relaxed) >= MAX_CONNS {
                continue; // dropped: too many at once
            }
            let Ok(local) = stream.local_addr() else { continue };
            // Only the local network (no firewall here: see the top). A new Wi-Fi address isn't in
            // the list for up to 20 s: looked up again before saying no.
            if !admit(local, peer, &lan.read().unwrap()) {
                let now = addresses();
                let ok = admit(local, peer, &now);
                *lan.write().unwrap() = now;
                if !ok {
                    say!("wi-fi: refused {} (to {}): not from this phone's local network", canonical(peer.ip()), canonical(local.ip()));
                    continue;
                }
            }
            let _ = stream.set_nodelay(true);
            let acceptor = TlsAcceptor::from(config.read().unwrap().clone());
            let app = app.clone();
            let open = open.clone();
            open.fetch_add(1, Ordering::Relaxed);
            tokio::spawn(async move {
                let peer = SocketAddr::new(canonical(peer.ip()), peer.port());
                if let Ok(Ok(tls)) = tokio::time::timeout(Duration::from_secs(10), acceptor.accept(stream)).await {
                    crate::http::serve_conn(app, tls, Door::Lan { peer, local }).await;
                }
                open.fetch_sub(1, Ordering::Relaxed);
            });
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hosts() {
        let local: SocketAddr = "10.0.0.30:7443".parse().unwrap();
        assert!(host_is_ours("10.0.0.30:7443", local, "galaxy-s24"));
        assert!(host_is_ours("Galaxy-S24.local:7443", local, "galaxy-s24"));
        assert!(host_is_ours("galaxy-s24:7443", local, "galaxy-s24"));
        assert!(!host_is_ours("10.0.0.30", local, "galaxy-s24"), "the port is part of it");
        assert!(!host_is_ours("10.0.0.31:7443", local, "galaxy-s24"));
        assert!(!host_is_ours("evil.example:7443", local, "galaxy-s24"));
        assert!(!host_is_ours(":7443", local, ""), "no name: only the address");
        // IPv4 on the dual-stack socket
        let mapped: SocketAddr = "[::ffff:10.0.0.30]:7443".parse().unwrap();
        assert!(host_is_ours("10.0.0.30:7443", mapped, ""));
        let v6: SocketAddr = "[fd00::1]:7443".parse().unwrap();
        assert!(host_is_ours("[fd00::1]:7443", v6, ""));
    }

    #[test]
    fn only_the_local_network() {
        let lan: Vec<Ipv4Addr> = vec!["10.0.0.30".parse().unwrap(), "172.16.42.1".parse().unwrap()];
        let to_wifi: SocketAddr = "10.0.0.30:7443".parse().unwrap();
        let to_usb: SocketAddr = "172.16.42.1:7443".parse().unwrap();
        let to_mobile: SocketAddr = "10.87.3.9:7443".parse().unwrap();
        let from = |s: &str| -> SocketAddr { s.parse().unwrap() };
        assert!(admit(to_wifi, from("10.0.0.20:50000"), &lan));
        assert!(admit(to_usb, from("172.16.42.2:50000"), &lan));
        assert!(admit(to_wifi, from("172.20.0.5:50000"), &lan), "another private network at school or work");
        assert!(admit(to_wifi, from("100.64.1.2:50000"), &lan), "Wi-Fi in the carriers' shared range");
        assert!(!admit(to_wifi, from("8.8.8.8:50000"), &lan), "a public address");
        assert!(!admit(to_mobile, from("172.20.0.5:50000"), &lan), "to an address that isn't a local network's");
        assert!(!admit(from("[2001:db8::1]:7443"), from("[2001:db8::2]:50000"), &lan), "IPv6");
        assert!(admit(from("[::ffff:10.0.0.30]:7443"), from("[::ffff:10.0.0.20]:1"), &lan), "IPv4 as IPv6");
    }

    /// At boot Remote Desktop starts before the Wi-Fi is up: the certificate made when the Wi-Fi was
    /// there must be kept (a new one would bring the browser's warning back), and a new network adds
    /// to it. On real files, in a folder of its own.
    #[test]
    fn certificate_kept_through_a_boot() {
        let dir = std::env::temp_dir().join(format!("s24-remote-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::env::set_var("XDG_DATA_HOME", &dir);
        let v = |s: &[&str]| s.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        let (first, _) = certificate(&v(&["galaxy-s24", "galaxy-s24.local", "10.0.0.30", "172.16.42.1"])).unwrap();
        // the Wi-Fi not up yet: only the USB address
        let (boot, _) = certificate(&v(&["galaxy-s24", "galaxy-s24.local", "172.16.42.1"])).unwrap();
        assert_eq!(boot[0].as_ref(), first[0].as_ref(), "the same certificate while the Wi-Fi isn't up");
        let (wifi, _) = certificate(&v(&["galaxy-s24", "galaxy-s24.local", "10.0.0.30", "172.16.42.1"])).unwrap();
        assert_eq!(wifi[0].as_ref(), first[0].as_ref(), "and once it is");
        // another network: a new certificate, for it and the old addresses
        let (other, _) = certificate(&v(&["galaxy-s24", "galaxy-s24.local", "10.9.0.4"])).unwrap();
        assert_ne!(other[0].as_ref(), first[0].as_ref());
        let names = std::fs::read_to_string(names_file()).unwrap();
        assert!(names.starts_with("galaxy-s24 galaxy-s24.local 10.9.0.4 10.0.0.30 172.16.42.1\n"), "{names}");
        let (back, _) = certificate(&v(&["galaxy-s24", "galaxy-s24.local", "10.0.0.30"])).unwrap();
        assert_eq!(back[0].as_ref(), other[0].as_ref(), "back home: no new one");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn certificate_names_kept() {
        let v = |s: &[&str]| s.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        // Wi-Fi not up yet at boot: the kept names (with it) cover what is needed, so the kept
        // certificate is used; a new network adds its address to the old ones
        assert_eq!(
            merged_names(&v(&["galaxy-s24", "galaxy-s24.local", "10.9.0.4"]), &v(&["galaxy-s24", "galaxy-s24.local", "10.0.0.30", "172.16.42.1"])),
            v(&["galaxy-s24", "galaxy-s24.local", "10.9.0.4", "10.0.0.30", "172.16.42.1"])
        );
        // at most MAX_CERT_IPS addresses, the current ones first
        let many: Vec<String> = (1..=12).map(|i| format!("10.0.{i}.1")).collect();
        let m = merged_names(&v(&["10.9.9.9"]), &many);
        assert_eq!(m.len(), MAX_CERT_IPS);
        assert_eq!(m[0], "10.9.9.9");
    }

    #[test]
    fn interfaces() {
        for n in ["wlan0", "usb0", "eth0", "enx001122334455"] {
            assert!(lan_interface(n), "{n}");
        }
        for n in ["lo", "rmnet_data0", "ccmni0", "dummy0", "tun0", "wwan0"] {
            assert!(!lan_interface(n), "{n}");
        }
    }

    #[test]
    fn names() {
        let ips = ["10.0.0.30".parse().unwrap(), "172.16.42.1".parse().unwrap()];
        assert_eq!(
            cert_names("galaxy-s24", &ips),
            vec!["galaxy-s24", "galaxy-s24.local", "10.0.0.30", "172.16.42.1"]
        );
        assert_eq!(cert_names("", &ips[..1]), vec!["10.0.0.30"]);
        assert_eq!(url_for(&ips, 7443).as_deref(), Some("https://10.0.0.30:7443"));
        assert_eq!(url_for(&[], 7443), None);
    }

    #[test]
    fn a_certificate_with_its_names() {
        let names = cert_names("galaxy-s24", &["10.0.0.30".parse().unwrap()]);
        let mut params = rcgen::CertificateParams::new(names).unwrap();
        params.extended_key_usages = vec![rcgen::ExtendedKeyUsagePurpose::ServerAuth];
        let key = rcgen::KeyPair::generate().unwrap();
        let cert = params.self_signed(&key).unwrap();
        // The IP went in as an IP address name, not a DNS name (browsers check that kind).
        assert!(params.subject_alt_names.iter().any(|n| matches!(n, rcgen::SanType::IpAddress(_))));
        assert!(params.subject_alt_names.iter().any(|n| matches!(n, rcgen::SanType::DnsName(_))));
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let cfg = ServerConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .unwrap()
            .with_no_client_auth()
            .with_single_cert(vec![cert.der().clone()], PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.serialize_der())));
        assert!(cfg.is_ok(), "rustls takes it: {:?}", cfg.err());
    }
}
