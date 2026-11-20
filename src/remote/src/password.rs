//! A password of the owner's choosing, besides the one-time code: for a browser to sign in without
//! looking at the phone (one that is far away, or a phone that restarted while Remote Desktop stays on).
//!
//! - Set from a signed-in browser (the page's menu) or at the phone (`s24-remote password`)
//! - At least MIN_LEN characters: with the sign-in limits in auth.rs (30 wrong tries an hour) that is
//!   out of reach of guessing even when it is all digits
//! - Kept as PBKDF2-HMAC-SHA256 with a random salt in ~/.local/share/s24-remote/password (0600),
//!   never as itself. Read again at every try, so `s24-remote password` takes effect at once

use std::num::NonZeroU32;
use std::path::PathBuf;

use ring::pbkdf2;

use crate::util::{data_dir, hex, random_bytes, write_private};

pub const MIN_LEN: usize = 8;
pub const MAX_LEN: usize = 128;
/// About a fifth of a second on the phone's fast cores: nothing next to the second a wrong try costs.
const ROUNDS: u32 = 150_000;

fn file() -> PathBuf {
    data_dir().join("password")
}

/// Whether a password is set.
pub fn is_set() -> bool {
    std::fs::read_to_string(file()).map(|t| parse(&t).is_some()).unwrap_or(false)
}

/// Why a password can't be the password, if it can't.
pub fn check_new(pw: &str) -> Result<(), String> {
    let n = pw.chars().count();
    if n < MIN_LEN {
        return Err(format!("At least {MIN_LEN} characters"));
    }
    if n > MAX_LEN {
        return Err(format!("At most {MAX_LEN} characters"));
    }
    if pw.chars().any(|c| c.is_control()) {
        return Err("Letters, digits, spaces and signs only".into());
    }
    Ok(())
}

pub fn set(pw: &str) -> Result<(), String> {
    check_new(pw)?;
    let line = hash_line(pw, &random_bytes(16), ROUNDS);
    write_private(&file(), line.as_bytes()).map_err(|e| format!("{}: {e}", file().display()))
}

pub fn remove() -> Result<(), String> {
    match std::fs::remove_file(file()) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("{}: {e}", file().display())),
    }
}

/// Whether `typed` is the password (false when none is set).
pub fn verify(typed: &str) -> bool {
    let Ok(text) = std::fs::read_to_string(file()) else { return false };
    let Some((rounds, salt, hash)) = parse(&text) else { return false };
    let Some(rounds) = NonZeroU32::new(rounds) else { return false };
    pbkdf2::verify(pbkdf2::PBKDF2_HMAC_SHA256, rounds, &salt, typed.as_bytes(), &hash).is_ok()
}

fn hash_line(pw: &str, salt: &[u8], rounds: u32) -> String {
    let mut out = [0u8; 32];
    pbkdf2::derive(pbkdf2::PBKDF2_HMAC_SHA256, NonZeroU32::new(rounds).unwrap(), salt, pw.as_bytes(), &mut out);
    format!("pbkdf2-sha256${rounds}${}${}\n", hex(salt), hex(&out))
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

/// "pbkdf2-sha256$rounds$salt$hash" → its parts.
fn parse(text: &str) -> Option<(u32, Vec<u8>, Vec<u8>)> {
    let mut parts = text.trim().split('$');
    if parts.next()? != "pbkdf2-sha256" {
        return None;
    }
    let rounds: u32 = parts.next()?.parse().ok()?;
    let salt = unhex(parts.next()?)?;
    let hash = unhex(parts.next()?)?;
    if parts.next().is_some() || salt.len() < 8 || hash.len() != 32 || rounds < 1000 {
        return None;
    }
    Some((rounds, salt, hash))
}

/// For tests: the same comparison as `verify`, on a line instead of the file.
#[cfg(test)]
fn verify_line(line: &str, typed: &str) -> bool {
    let Some((rounds, salt, hash)) = parse(line) else { return false };
    let mut out = [0u8; 32];
    pbkdf2::derive(pbkdf2::PBKDF2_HMAC_SHA256, NonZeroU32::new(rounds).unwrap(), &salt, typed.as_bytes(), &mut out);
    crate::util::same_secret(&out, &hash)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_and_check() {
        let line = hash_line("correct horse", b"0123456789abcdef", 2000);
        assert!(line.starts_with("pbkdf2-sha256$2000$"));
        assert!(!line.contains("correct horse"));
        assert!(verify_line(&line, "correct horse"));
        assert!(!verify_line(&line, "correct horsE"));
        assert!(!verify_line(&line, ""));
        // the same password with another salt is another hash
        assert_ne!(line, hash_line("correct horse", b"fedcba9876543210", 2000));
    }

    #[test]
    fn rules_for_a_new_one() {
        assert!(check_new("1234567").is_err());
        assert!(check_new("12345678").is_ok());
        assert!(check_new("a sentence, with spaces").is_ok());
        assert!(check_new(&"x".repeat(MAX_LEN + 1)).is_err());
        assert!(check_new("tab\there").is_err());
    }

    #[test]
    fn broken_lines_are_no_password() {
        assert!(parse("").is_none());
        assert!(parse("plain$1$00$00").is_none());
        assert!(parse("pbkdf2-sha256$10$0011223344556677$00").is_none());
        assert!(parse("pbkdf2-sha256$2000$zz$00").is_none());
    }
}
