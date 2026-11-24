const assert = require('node:assert/strict');
const { test } = require('node:test');
const { ALT, CTRL, SHIFT, QT_KEY, accelerators, shortcut, remoteKey } = require('../keyboard');

test('the window advertises platform-specific accelerators', () => {
  assert.deepEqual(accelerators(false), { desktop: 'Ctrl+Alt+1', phone: 'Ctrl+Alt+2', hud: 'Ctrl+Alt+I' });
  assert.equal(accelerators(true).desktop, 'Cmd+1');
});

test('window commands leave ordinary desktop Ctrl shortcuts available', () => {
  const chord = { ctrlKey: true, altKey: true };
  assert.equal(shortcut({ ...chord, code: 'Digit1' }, true), 'desktop');
  assert.equal(shortcut({ ...chord, code: 'Digit2' }, true), 'phone');
  assert.equal(shortcut({ ...chord, code: 'KeyI' }, true), 'hud');
  for (const code of ['KeyA', 'KeyC', 'KeyZ', 'KeyX', 'KeyI', 'Digit1']) {
    const event = { ctrlKey: true, code, key: 'ы' };
    assert.equal(shortcut(event, true), null);
    assert.deepEqual(remoteKey(event), [code.charCodeAt(code.length - 1), CTRL, '']);
  }
  assert.equal(shortcut({ ctrlKey: true, code: 'KeyV' }, true), 'paste');
  assert.equal(shortcut({ ctrlKey: true, shiftKey: true, code: 'KeyV' }, true), null);
});

test('phone navigation and desktop file and screenshot keys use the same Qt codes as the shell', () => {
  for (const [key, action] of [['ArrowUp', 'home'], ['ArrowDown', 'shade']]) {
    const event = { key, code: key, altKey: true };
    assert.equal(shortcut(event, false), action);
    assert.equal(shortcut(event, true), null);
    assert.deepEqual(remoteKey(event), [QT_KEY[key], ALT, '']);
  }
  assert.equal(shortcut({ key: 'Escape', code: 'Escape' }, false), 'back');
  assert.equal(shortcut({ key: 'Escape', code: 'Escape' }, true), null);
  assert.deepEqual(remoteKey({ key: '№', code: 'Digit3', altKey: true, shiftKey: true }), [0x33, ALT | SHIFT, '']);
  assert.deepEqual(remoteKey({ key: 'Backspace', code: 'Backspace', altKey: true }), [QT_KEY.Backspace, ALT, '']);
});

test('AltGr, Unicode text and the system modifier stay distinct from window shortcuts', () => {
  const altGr = { key: '€', code: 'KeyI', ctrlKey: true, altKey: true, getModifierState: (name) => name === 'AltGraph' };
  assert.equal(shortcut(altGr, true), null);
  assert.deepEqual(remoteKey(altGr), [0, 0, '€']);
  assert.deepEqual(remoteKey({ key: 'Я', code: 'KeyZ', shiftKey: true }), [0, 0, 'Я']);
  assert.equal(remoteKey({ key: 'z', code: 'KeyZ', metaKey: true }), null);
});
