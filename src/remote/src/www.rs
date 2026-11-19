//! The page, built into the program: nothing to install beside it, nothing a browser loads from
//! anywhere else.

pub fn file(path: &str) -> Option<(&'static str, &'static [u8])> {
    Some(match path {
        "/" | "/index.html" => ("text/html; charset=utf-8", include_bytes!("../www/index.html")),
        "/app.js" => ("text/javascript; charset=utf-8", include_bytes!("../www/app.js")),
        "/keys.js" => ("text/javascript; charset=utf-8", include_bytes!("../www/keys.js")),
        "/style.css" => ("text/css; charset=utf-8", include_bytes!("../www/style.css")),
        "/icon.svg" => ("image/svg+xml", include_bytes!("../www/icon.svg")),
        _ => return None,
    })
}
