//! Who may come in: a code shown only on the phone, traded once for a token the browser keeps.
//!
//! - The code is 8 digits, from the kernel's random source, and works once: after a browser signs in
//!   with it, the phone shows a new one for the next browser
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
    /// Signed in: the token for the cookie.
    Ok(String),
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

    /// One try. Digits only count: spaces and dashes people type are taken out first.
    pub fn login(&mut self, typed: &str, who: &str, now: Instant) -> Login {
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
        let digits: String = typed.chars().filter(|c| c.is_ascii_digit()).collect();
        if same_secret(digits.as_bytes(), self.code.as_bytes()) {
            self.wrong = 0;
            self.replaced = 0;
            self.code = new_code(); // used up: the next browser gets the next code
            let value = hex(&random_bytes(32));
            if self.tokens.len() >= MAX_TOKENS {
                self.tokens.pop_front();
            }
            self.tokens.push_back(Token { value: value.clone(), who: who.to_string() });
            return Login::Ok(value);
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
        let Login::Ok(token) = a.login(&typed, "1.2.3.4", now) else { panic!("not signed in") };
        assert_eq!(token.len(), 64);
        assert_eq!(a.check(&token), Some("1.2.3.4"));
        assert_ne!(a.code(), code, "a used code is replaced");
        assert!(matches!(a.login(&code, "x", now), Login::Wrong { .. }), "the old code is spent");
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
                assert_eq!(a.login(&w, "x", now), Login::Wrong { left: WRONG_PER_CODE - i, replaced: false });
            }
            let w = wrong(&a);
            let r = a.login(&w, "x", now);
            if round + 1 < CODES_BEFORE_LOCK {
                assert_eq!(r, Login::Wrong { left: WRONG_PER_CODE, replaced: true });
                assert_ne!(a.code(), before);
            } else {
                assert_eq!(r, Login::Locked { secs: LOCK_FOR.as_secs() });
            }
        }
        // Locked: even the right code is refused.
        let code = a.code().to_string();
        assert!(matches!(a.login(&code, "x", now), Login::Locked { .. }));
        // An hour later: a fresh code, and it works.
        let later = now + LOCK_FOR + Duration::from_secs(1);
        assert!(matches!(a.login("nope", "x", later), Login::Wrong { left: 9, .. }));
        let code = a.code().to_string();
        assert!(matches!(a.login(&code, "x", later), Login::Ok(_)));
    }

    #[test]
    fn tokens_are_capped() {
        let mut a = Auth::new();
        let now = Instant::now();
        let mut tokens = vec![];
        for i in 0..MAX_TOKENS + 1 {
            let code = a.code().to_string();
            let Login::Ok(t) = a.login(&code, &format!("b{i}"), now) else { panic!() };
            tokens.push(t);
        }
        assert_eq!(a.check(&tokens[0]), None, "the oldest went");
        assert_eq!(a.check(&tokens[MAX_TOKENS]), Some(format!("b{MAX_TOKENS}").as_str()));
    }
}
