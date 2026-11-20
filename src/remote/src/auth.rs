//! Who may come in: a code shown only on the phone, or the owner's password, traded for a token the
//! browser keeps.
//!
//! - The code is 8 digits, from the kernel's random source, and works once: after a browser signs in
//!   with it, the phone shows a new one for the next browser
//! - The password, if the owner set one (password.rs), works every time; wrong passwords count as
//!   wrong codes below
//! - 10 wrong codes in a row and the code is replaced (the phone shows the new one); the third time
//!   that happens, nobody can sign in for an hour (turning Remote Desktop off and on ends that).
//!   Whoever guesses gets 30 tries an hour at a hundred million codes, from however many addresses
//! - A failed try takes a second and tries are taken one at a time (see http.rs), so many
//!   connections at once get no more tries than one
//! - The token is 256 bits, kept only in this program's memory and in the browser's cookie; at most
//!   MAX_TOKENS browsers are signed in, the oldest goes first. All of them end when Remote Desktop
//!   is turned off

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use crate::util::{hex, random_below, random_bytes, same_secret};

pub const CODE_DIGITS: usize = 8;
pub const WRONG_PER_CODE: u32 = 10;
pub const CODES_BEFORE_LOCK: u32 = 3;
pub const LOCK_FOR: Duration = Duration::from_secs(3600);
pub const MAX_TOKENS: usize = 4;

#[derive(Debug, PartialEq)]
pub enum Login {
    /// Signed in: the token for the cookie, and whether it was the password (not the code).
    Ok { token: String, password: bool },
    /// Wrong; this many tries are left on this code before it is replaced.
    Wrong { left: u32, replaced: bool },
    /// Too many wrong codes: no sign-in for this many seconds.
    Locked { secs: u64 },
}

struct Token {
    value: String,
    who: String,
}

pub struct Auth {
    code: String,
    wrong: u32,
    replaced: u32,
    locked_until: Option<Instant>,
    tokens: VecDeque<Token>,
}

fn new_code() -> String {
    format!("{:0width$}", random_below(10u32.pow(CODE_DIGITS as u32)), width = CODE_DIGITS)
}

/// "12345678" as people read it: "1234 5678".
pub fn spaced(code: &str) -> String {
    if code.len() == 8 {
        format!("{} {}", &code[..4], &code[4..])
    } else {
        code.to_string()
    }
}

impl Default for Auth {
    fn default() -> Self {
        Self::new()
    }
}

impl Auth {
    pub fn new() -> Auth {
        Auth { code: new_code(), wrong: 0, replaced: 0, locked_until: None, tokens: VecDeque::new() }
    }

    pub fn code(&self) -> &str {
        &self.code
    }

    pub fn locked_for(&self, now: Instant) -> Option<Duration> {
        self.locked_until.filter(|t| *t > now).map(|t| t - now)
    }

    /// Whether `typed` is the code (digits only count: spaces and dashes people type are taken out).
    pub fn is_code(&self, typed: &str) -> bool {
        let digits: String = typed.chars().filter(|c| c.is_ascii_digit()).collect();
        same_secret(digits.as_bytes(), self.code.as_bytes())
    }

    /// One try: the code, or the password. `password_ok` is password::verify's answer for what was
    /// typed (worked out beforehand, off the thread that carries the picture: it takes a moment).
    pub fn login(&mut self, typed: &str, password_ok: bool, who: &str, now: Instant) -> Login {
        if let Some(left) = self.locked_for(now) {
            return Login::Locked { secs: left.as_secs().max(1) };
        }
        if self.locked_until.is_some() {
            // The hour is over: a fresh start.
            self.locked_until = None;
            self.replaced = 0;
            self.wrong = 0;
            self.code = new_code();
        }
        let by_code = self.is_code(typed);
        let by_password = !by_code && !typed.is_empty() && password_ok;
        if by_code || by_password {
            self.wrong = 0;
            self.replaced = 0;
            if by_code {
                self.code = new_code(); // used up: the next browser gets the next code
            }
            let value = hex(&random_bytes(32));
            if self.tokens.len() >= MAX_TOKENS {
                self.tokens.pop_front();
            }
            self.tokens.push_back(Token { value: value.clone(), who: who.to_string() });
            return Login::Ok { token: value, password: by_password };
        }
        self.wrong += 1;
        if self.wrong >= WRONG_PER_CODE {
            self.wrong = 0;
            self.replaced += 1;
            self.code = new_code();
            if self.replaced >= CODES_BEFORE_LOCK {
                self.locked_until = Some(now + LOCK_FOR);
                return Login::Locked { secs: LOCK_FOR.as_secs() };
            }
            return Login::Wrong { left: WRONG_PER_CODE, replaced: true };
        }
        Login::Wrong { left: WRONG_PER_CODE - self.wrong, replaced: false }
    }

    /// The browser behind this token, if it is one of ours.
    pub fn check(&self, token: &str) -> Option<&str> {
        self.tokens
            .iter()
            .find(|t| same_secret(t.value.as_bytes(), token.as_bytes()))
            .map(|t| t.who.as_str())
    }

    pub fn logout(&mut self, token: &str) {
        self.tokens.retain(|t| !same_secret(t.value.as_bytes(), token.as_bytes()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn password_signs_in_without_using_up_the_code() {
        let mut a = Auth::new();
        let now = Instant::now();
        let code = a.code().to_string();
        let Login::Ok { password: true, .. } = a.login("open sesame 42", true, "x", now) else { panic!("password refused") };
        assert_eq!(a.code(), code, "the code stays for the next browser");
        let Login::Ok { password: true, .. } = a.login("open sesame 42", true, "y", now) else { panic!("password works again") };
        assert!(matches!(a.login("open sesame 43", false, "z", now), Login::Wrong { left: 9, .. }), "a wrong password is a wrong try");
        assert!(matches!(a.login("", true, "z", now), Login::Wrong { .. }), "nothing typed is never right");
        assert!(a.is_code(&code) && !a.is_code("open sesame 42"));
        let Login::Ok { password: false, .. } = a.login(&code, false, "x", now) else { panic!("the code still works") };
        assert_ne!(a.code(), code, "and is used up");
    }

    #[test]
    fn code_shape() {
        let a = Auth::new();
        assert_eq!(a.code().len(), 8);
        assert!(a.code().chars().all(|c| c.is_ascii_digit()));
        assert_eq!(spaced("12345678"), "1234 5678");
    }

    #[test]
    fn right_code_signs_in_once() {
        let mut a = Auth::new();
        let now = Instant::now();
        let code = a.code().to_string();
        let typed = format!("{} {}", &code[..4], &code[4..]);
        let Login::Ok { token, password: false } = a.login(&typed, false, "1.2.3.4", now) else { panic!("not signed in") };
        assert_eq!(token.len(), 64);
        assert_eq!(a.check(&token), Some("1.2.3.4"));
        assert_ne!(a.code(), code, "a used code is replaced");
        assert!(matches!(a.login(&code, false, "x", now), Login::Wrong { .. }), "the old code is spent");
        a.logout(&token);
        assert_eq!(a.check(&token), None);
    }

    #[test]
    fn wrong_codes_replace_then_lock() {
        let mut a = Auth::new();
        let now = Instant::now();
        let wrong = |a: &Auth| if a.code() == "00000000" { "11111111".to_string() } else { "00000000".to_string() };
        for round in 0..CODES_BEFORE_LOCK {
            let before = a.code().to_string();
            for i in 1..WRONG_PER_CODE {
                let w = wrong(&a);
                assert_eq!(a.login(&w, false, "x", now), Login::Wrong { left: WRONG_PER_CODE - i, replaced: false });
            }
            let w = wrong(&a);
            let r = a.login(&w, false, "x", now);
            if round + 1 < CODES_BEFORE_LOCK {
                assert_eq!(r, Login::Wrong { left: WRONG_PER_CODE, replaced: true });
                assert_ne!(a.code(), before);
            } else {
                assert_eq!(r, Login::Locked { secs: LOCK_FOR.as_secs() });
            }
        }
        // Locked: even the right code is refused.
        let code = a.code().to_string();
        assert!(matches!(a.login(&code, false, "x", now), Login::Locked { .. }));
        // An hour later: a fresh code, and it works.
        let later = now + LOCK_FOR + Duration::from_secs(1);
        assert!(matches!(a.login("nope", false, "x", later), Login::Wrong { left: 9, .. }));
        let code = a.code().to_string();
        assert!(matches!(a.login(&code, false, "x", later), Login::Ok { .. }));
    }

    #[test]
    fn tokens_are_capped() {
        let mut a = Auth::new();
        let now = Instant::now();
        let mut tokens = vec![];
        for i in 0..MAX_TOKENS + 1 {
            let code = a.code().to_string();
            let Login::Ok { token: t, .. } = a.login(&code, false, &format!("b{i}"), now) else { panic!() };
            tokens.push(t);
        }
        assert_eq!(a.check(&tokens[0]), None, "the oldest went");
        assert_eq!(a.check(&tokens[MAX_TOKENS]), Some(format!("b{MAX_TOKENS}").as_str()));
    }
}
