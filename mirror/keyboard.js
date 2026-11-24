const SHIFT = 0x02000000, CTRL = 0x04000000, ALT = 0x08000000;
const QT_KEY = {
  Escape: 0x01000000, Tab: 0x01000001, Backspace: 0x01000003, Enter: 0x01000004, Delete: 0x01000007,
  Home: 0x01000010, End: 0x01000011, ArrowLeft: 0x01000012, ArrowUp: 0x01000013, ArrowRight: 0x01000014,
  ArrowDown: 0x01000015, PageUp: 0x01000016, PageDown: 0x01000017, F2: 0x01000031, F5: 0x01000034,
  F10: 0x01000039, ContextMenu: 0x01000055,
};
const MODIFIER_KEYS = new Set(['Shift', 'Control', 'Alt', 'AltGraph', 'Meta', 'CapsLock']);
const MODIFIER_CODES = { Shift: 0x01000020, Control: 0x01000021, Alt: 0x01000023 };
const WINDOW_KEYS = { Digit1: 'desktop', Digit2: 'phone', KeyI: 'hud' };
const ALT_NAV = { ArrowUp: 'home', ArrowDown: 'shade' };

function accelerators(mac) {
  const mod = mac ? 'Cmd' : 'Ctrl+Alt';
  return { desktop: mod + '+1', phone: mod + '+2', hud: mod + '+I' };
}

function altGraph(event) {
  return event.getModifierState?.('AltGraph') === true;
}

// Сочетания без Cmd (Windows/Linux). AltGr даёт символы раскладки, в том числе при Ctrl+Alt в DOM.
function shortcut(event, desktop) {
  if (event.metaKey || event.shiftKey || altGraph(event)) return null;
  if (event.ctrlKey && event.altKey) return WINDOW_KEYS[event.code] || null;
  if (event.ctrlKey && event.code === 'KeyV') return 'paste';
  if (!desktop && event.altKey) return ALT_NAV[event.code] || null;
  if (!desktop && !event.ctrlKey && event.key === 'Escape') return 'back';
  return null;
}

function remoteKey(event) {
  const { key, code } = event;
  if (event.metaKey || event.isComposing) return null;
  if (altGraph(event)) return [...key].length === 1 ? [0, 0, key] : null;
  const mods = (event.shiftKey ? SHIFT : 0) | (event.ctrlKey ? CTRL : 0) | (event.altKey ? ALT : 0);
  if (event.ctrlKey || event.altKey) {
    // Сочетание с буквой или цифрой передаётся по физической клавише при любой раскладке.
    if (/^(Key[A-Z]|Digit[0-9])$/.test(code)) return [code.charCodeAt(code.length - 1), mods, ''];
  }
  if (QT_KEY[key] || MODIFIER_CODES[key])
    return [QT_KEY[key] || MODIFIER_CODES[key], mods, key === 'Enter' ? '\r' : key === 'Tab' ? '\t' : ''];
  if (/^F([1-9]|1[0-2])$/.test(key)) return [0x01000030 + Number(key.slice(1)) - 1, mods, ''];
  return [...key].length === 1 ? [0, 0, key] : null;
}

module.exports = { CTRL, ALT, SHIFT, QT_KEY, MODIFIER_KEYS, accelerators, shortcut, remoteKey };
