// Keys as the phone takes them: X keysyms. The phone's helper finds each one on the phone's own
// keyboard layout and presses it there, adding Shift or AltGr when the character needs them.
//
// - A character key sends the character it typed here (event.key), so whatever this computer's
//   layout is, the same character comes out on the phone
// - With Ctrl, Alt or Cmd held it is a shortcut: the key's place on a US keyboard (event.code), so
//   Ctrl+C is Ctrl+C on every layout (AltGr, which Windows makes from Ctrl+Alt, still types)
// - Dead keys send nothing; the character they make comes with the next key
// - On a Mac, Cmd is the phone's Ctrl: Cmd+C copies there as it does here

export const isMac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);

const NAMED = {
  Enter: 0xff0d, Tab: 0xff09, Backspace: 0xff08, Escape: 0xff1b, Delete: 0xffff, Insert: 0xff63,
  Home: 0xff50, End: 0xff57, PageUp: 0xff55, PageDown: 0xff56,
  ArrowLeft: 0xff51, ArrowUp: 0xff52, ArrowRight: 0xff53, ArrowDown: 0xff54,
  Pause: 0xff13, ScrollLock: 0xff14, PrintScreen: 0xff61, ContextMenu: 0xff67,
  NumLock: 0xff7f, CapsLock: 0xffe5, Clear: 0xff0b,
  AudioVolumeUp: 0x1008ff13, AudioVolumeDown: 0x1008ff11, AudioVolumeMute: 0x1008ff12,
  MediaPlayPause: 0x1008ff14, MediaStop: 0x1008ff15, MediaTrackPrevious: 0x1008ff16, MediaTrackNext: 0x1008ff17,
  BrowserBack: 0x1008ff26, BrowserForward: 0x1008ff27, BrowserRefresh: 0x1008ff29, BrowserHome: 0x1008ff18,
};
for (let i = 1; i <= 24; i++) NAMED['F' + i] = 0xffbe + i - 1;

const MODS = {
  ShiftLeft: 0xffe1, ShiftRight: 0xffe2, ControlLeft: 0xffe3, ControlRight: 0xffe4,
  AltLeft: 0xffe9, AltRight: 0xffea, MetaLeft: 0xffeb, MetaRight: 0xffec, OSLeft: 0xffeb, OSRight: 0xffec,
};

// A US keyboard's characters by place, for shortcuts.
const BASE = {
  Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\', Semicolon: ';',
  Quote: "'", Comma: ',', Period: '.', Slash: '/', Backquote: '`', Space: ' ', IntlBackslash: '<',
};
for (let c = 65; c <= 90; c++) BASE['Key' + String.fromCharCode(c)] = String.fromCharCode(c + 32);
for (let d = 0; d <= 9; d++) BASE['Digit' + d] = String(d);

/** A character's keysym: Latin-1 as itself, anything else as 0x01000000 + its code point. */
export function charKeysym(ch) {
  const cp = ch.codePointAt(0);
  if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)) return cp;
  if (cp === 0x0a || cp === 0x0d) return 0xff0d;
  if (cp === 0x09) return 0xff09;
  return 0x01000000 + cp;
}

/** The keysym to press for this keydown, or 0 to leave the key alone. */
export function keysymFor(e) {
  if (isMac && (e.code === 'MetaLeft' || e.code === 'MetaRight')) return e.code === 'MetaLeft' ? 0xffe3 : 0xffe4;
  if (MODS[e.code]) return MODS[e.code];
  switch (e.key) {
    case 'AltGraph': return 0xfe03;
    case 'Shift': return 0xffe1;
    case 'Control': return 0xffe3;
    case 'Alt': return 0xffe9;
    case 'Meta': case 'OS': case 'Super': return isMac ? 0xffe3 : 0xffeb;
    case 'Dead': case 'Process': case 'Unidentified': case 'Compose': return 0;
  }
  if (e.code === 'NumpadEnter') return 0xff8d;
  if (NAMED[e.key] !== undefined) return NAMED[e.key];
  const altGr = typeof e.getModifierState === 'function' && e.getModifierState('AltGraph');
  const shortcut = (e.ctrlKey || e.altKey || e.metaKey) && !altGr;
  if (shortcut && BASE[e.code]) return charKeysym(BASE[e.code]);
  if ([...e.key].length === 1) return charKeysym(e.key);
  return 0;
}
