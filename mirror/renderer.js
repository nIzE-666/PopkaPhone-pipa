// НаомиФон Mirror — страница окна (docs/STREAM.md, «Окно на Mac»; рабочий стол — docs/DESKTOP.md):
//  • что смотреть — рабочий стол (по умолчанию: второй экран телефона, 16:9, мышь как мышь) или экран телефона
//    (мышь как палец); меню «Вид», выбор помнится;
//  • связь — SSH ключом разработчика (private/ssh) или локальный TCP через adb forward; два пути одного сеанса
//    позволяют переключаться между кабелем и Wi-Fi (keepRoute, «Пути» ниже);
//    служба трансляции слушает только у себя, открытого порта у телефона нет;
//  • видео — H.264 или HEVC Annex B через WebCodecs, кадр — на холст в обновление
//    экрана, каждый в своё (после провала связи — догоняет, не больше чем на 3 кадра);
//  • ввод — мышь (на экране телефона — как палец), прокрутка трекпадом — колесо, клавиатура — текст и клавиши;
//    у экрана телефона Esc — «назад»; сочетания Windows/Linux — в меню «Справка».
//  • NAOMI_MIRROR_VIA=wifi — по Wi-Fi, даже когда есть кабель;
//  • экраны (docs/SCREENS.md): окно — один из экранов стола («mac»). Стол на него не выведен или выключен (Switch 0)
//    — заставка без дока: обои стола и часы, как у приставки, один тихий путь (пинг раз в 30 с), ввод не идёт; щелчок
//    или клавиша — «стол сюда» (Wake). Вывели (Switch 1) — подключиться заново сразу, заставка тает в картинку;
//    мышь другого экрана (приставки) видна стрелкой (CursorAt).
// Протокол — shell/stream/stream_protocol.h.
const { execFile, spawn } = require('child_process');
const { pathToFileURL } = require('url');
const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { ipcRenderer } = require('electron');
const { connectionOptions, sshArguments } = require('./connection');
const { message, input, readMessages } = require('./protocol');
const { annexBCodec, decoderConfig, isHevc, windowsCodec } = require('./video');
const { CTRL, ALT, SHIFT, QT_KEY, MODIFIER_KEYS, shortcut, remoteKey } = require('./keyboard');
const IS_WINDOWS = process.platform === 'win32';
const IS_MAC = process.platform === 'darwin';
const PLATFORM_NAME = IS_WINDOWS ? 'Windows' : IS_MAC ? 'Mac' : 'Linux';

// Репозиторий с ключом SSH и адресом Wi-Fi: из launcher, home.json у Mac или папки выше для исходников.
const ROOT = process.env.NAOMI_MIRROR_ROOT || (() => {
  try { return JSON.parse(fs.readFileSync(path.join(__dirname, 'home.json'), 'utf8')).root; } catch { return path.resolve(__dirname, '..'); }
})();
const CONNECTION = connectionOptions(process.env, process.platform);
const CABLE = '10.77.0.1';
const T = { HELLO: 1, VIDEO: 2, PONG: 3, ROUTE: 4, SWITCH: 9, PING: 10, TOUCH: 11, SCROLL: 12, KEY: 13, KEYFRAME: 14,
  NAV: 15, VIEWER: 16, MODE: 17, POINTER: 18, JOIN: 19, USE: 20, INPUT: 21, DESKSIZE: 22, WANT: 23, AWAY: 27,
  REVEAL: 28, AWAYSHOWN: 29, WAKE: 30, SCREEN: 32, CURSOR: 34, SCREENS: 38, SHOW: 39, CURSORAT: 40, WALLPAPER: 41 };
// форма курсора стола (Cursor, stream_protocol.h CursorShape) — системным курсором macOS: края окон, текст, ссылки
const CURSOR_CSS = ['default', 'pointer', 'text', 'ew-resize', 'ns-resize', 'nwse-resize', 'nesw-resize', 'move',
  'grabbing', 'progress', 'not-allowed', 'crosshair', 'none'];
const PARKED_PING_MS = 30000, PARKED_SILENT_MS = 95000;   // тихий путь (stream_protocol.h: kParkedPingS, kParkedSilentS)
const NAV = { BACK: 1, HOME: 2, SHADE: 3 };

const canvas = document.getElementById('screen');
// без desynchronized: с ним Chromium рисует холст своим слоем, и на кадре 1920 × 1080 окно застревало на первом
// кадре (кадры распаковывались, на экране — старый; 13 ноября 2026)
const ctx = canvas.getContext('2d', { alpha: false });
const statusEl = document.getElementById('status');
const hudEl = document.getElementById('hud');

const nowUs = () => Math.round(performance.now() * 1000);
let via = '';
let hello = null, decoder = null, waitingKey = true, shown = false;
let entering = false;               // поток начался: его первый кадр на экране — заставка тает (и Reveal телефону)
const awayMarks = new Map();        // метка захвата → фаза ухода стола (Away), до её кадра
let clockLater = null;              // часы ждут, вернётся ли стол сам (метка AwayBack)
let lostSince = 0;                  // связи нет с этого времени: дольше 1,5 с — заставка
const pending = new Map();          // метка захвата (часы телефона) → когда пришёл кадр, когда сжат
let pongs = [], offsetUs = null;    // часы телефона − часы Mac (лучший из последних пингов)
// что смотрим: 'desktop' — рабочий стол, 'phone' — экран телефона
let mode = (() => { try { return localStorage.getItem('mode') || 'desktop'; } catch { return 'desktop'; } })();
const desktop = () => mode === 'desktop';
function setMode(m) {
  mode = m;
  try { localStorage.setItem('mode', m); } catch {}
  send(T.MODE, Buffer.from([m === 'desktop' ? 1 : 0]));
}
ipcRenderer.on('mode', (_e, m) => setMode(m));
ipcRenderer.on('toggle-hud', () => document.body.classList.toggle('hud'));

// Форма стола — под окно (docs/DESKTOP.md, «Форма стола»): ширина стола та же (1280 точек, кадр 1920), высота — по
// окну, от 16:9 до 4:3, кратная 4. Окно поменяли (тянут край, полный экран) — через 250 мс после последнего
// изменения телефон перестраивает стол и шлёт новый Hello; пока тянут — полосы, как раньше.
function deskHeight() {
  const h = Math.round(1280 * innerHeight / innerWidth / 4) * 4;
  return Math.min(960, Math.max(720, h));
}
function syncDesk(force) {
  if (!desktop()) return;
  const h = deskHeight();
  if (!force && hello && hello.mode === 'desktop' && hello.height === h * 3 / 2) return;   // уже такой
  const b = Buffer.alloc(2);
  b.writeUInt16LE(h, 0);
  send(T.DESKSIZE, b);
}
let deskLater = null;
addEventListener('resize', () => {
  clearTimeout(deskLater);
  deskLater = setTimeout(() => syncDesk(false), 250);
});

function status(text) {
  statusEl.textContent = text || '';
  statusEl.style.display = text ? 'flex' : 'none';
}

// ---------- заставка без дока (docs/SCREENS.md) ----------
// Пока стол на это окно не выведен — обои стола во всё окно и часы с датой: тот же вид и то же место, что у приставки
// (pi/dock/clock.cpp: цифры Google Sans Flex 150 точек стола, тонкие, скруглённые; дата капсом 19 точек; место —
// левый верх чернил в долях экрана из настроек «Без дока», сообщение Screen). Обои телефон присылает сам (Wallpaper),
// они и настройки помнятся до следующего запуска. Обои и часы проявляются и тают порознь: стол уходит с экрана —
// сначала часы (в те же кадры, что панель и окна, — метки Away), потом обои; приходит — наоборот.
const wallEl = document.getElementById('wall'), clockEl = document.getElementById('clock');
const inkEl = document.getElementById('clockInk'), timeEl = document.getElementById('time'), dateEl = document.getElementById('date');
const guestEl = document.getElementById('guest');
const USER_DATA = ipcRenderer.sendSync('user-data');
const WALL_FILE = path.join(USER_DATA, 'wall.jpg');
const EASE = 'cubic-bezier(0.33, 1, 0.68, 1)';   // OutCubic, как анимации стола
const DAYS = ['ВОСКРЕСЕНЬЕ', 'ПОНЕДЕЛЬНИК', 'ВТОРНИК', 'СРЕДА', 'ЧЕТВЕРГ', 'ПЯТНИЦА', 'СУББОТА'];
const MONTHS = ['ЯНВАРЯ', 'ФЕВРАЛЯ', 'МАРТА', 'АПРЕЛЯ', 'МАЯ', 'ИЮНЯ', 'ИЮЛЯ', 'АВГУСТА', 'СЕНТЯБРЯ', 'ОКТЯБРЯ', 'НОЯБРЯ', 'ДЕКАБРЯ'];
const stored = (key, fallback) => { try { return JSON.parse(localStorage.getItem(key)) || fallback; } catch { return fallback; } };
// настройки экрана стола (Screen): часы есть, где они (1/10000 экрана), ширина стола в точках — размер часов
let screenCfg = stored('screenCfg', { clock: 1, clockX: 275, clockY: 378, width: 1600 });
let wallHash = '';
try { wallHash = localStorage.getItem('wall') || ''; } catch {}

function applyWall(jpeg) {
  wallEl.style.backgroundImage = jpeg && jpeg.length
    ? `url(${URL.createObjectURL(new Blob([jpeg], { type: 'image/jpeg' }))})` : 'linear-gradient(#1b1424, #08070b)';
}
if (wallHash && wallHash !== 'none') {
  try { applyWall(fs.readFileSync(WALL_FILE)); } catch { wallHash = ''; }
}

// шрифты часов — те же файлы, что у телефона и приставки (downloads/fonts); нет их — системный
for (const [family, file] of [['Google Sans Flex', 'GoogleSansFlex[ROND,opsz,slnt,wdth,wght].ttf'],
  ['Google Sans', 'GoogleSans[GRAD,opsz,wght].ttf']]) {
  const f = path.join(ROOT, 'downloads', 'fonts', file);
  if (fs.existsSync(f))
    new FontFace(family, `url("${pathToFileURL(f).href}")`, { weight: '100 1000' }).load()
      .then((face) => { document.fonts.add(face); layoutClock(); }).catch(() => {});
}

const measureCtx = document.createElement('canvas').getContext('2d');
function ink(font, text, spacing) {
  measureCtx.font = font;
  measureCtx.letterSpacing = `${spacing || 0}px`;
  return measureCtx.measureText(text);
}
let clockMinute = -1, clockShift = 8;
function layoutClock() {
  const now = new Date();
  clockMinute = now.getHours() * 60 + now.getMinutes();
  const W = innerWidth, H = innerHeight, pt = W / (screenCfg.width || 1600);
  const tpx = 150 * pt, dpx = 19 * pt, opsz = (v) => Math.min(144, Math.max(6, v));
  const hm = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  const date = `${DAYS[now.getDay()]} · ${now.getDate()} ${MONTHS[now.getMonth()]}`;
  timeEl.textContent = hm;
  dateEl.textContent = date;
  timeEl.style.fontSize = `${tpx}px`;
  timeEl.style.fontVariationSettings = `"ROND" 100, "opsz" ${opsz(tpx)}`;
  dateEl.style.fontSize = `${dpx}px`;
  dateEl.style.letterSpacing = `${0.1 * dpx}px`;
  dateEl.style.fontVariationSettings = `"opsz" ${opsz(dpx)}`;
  // место — по чернилам: левый верх чернил цифр — доля окна; дата — под цифрами через 16 точек
  const t = ink(`300 ${tpx}px "Google Sans Flex"`, hm), d = ink(`500 ${dpx}px "Google Sans"`, date, 0.1 * dpx);
  const left = screenCfg.clockX / 10000 * W, top = screenCfg.clockY / 10000 * H;
  const timeH = t.actualBoundingBoxAscent + t.actualBoundingBoxDescent;
  timeEl.setAttribute('x', left + t.actualBoundingBoxLeft);
  timeEl.setAttribute('y', top + t.actualBoundingBoxAscent);
  dateEl.setAttribute('x', left + d.actualBoundingBoxLeft);
  dateEl.setAttribute('y', top + timeH + 16 * pt + d.actualBoundingBoxAscent);
  inkEl.style.filter = `drop-shadow(0 ${2 * pt}px ${4 * pt}px rgba(0, 0, 0, .42))`;
  clockShift = 0.04 * (timeH + 16 * pt + d.actualBoundingBoxAscent + d.actualBoundingBoxDescent);
  if (!clockOn) clockEl.style.transform = `translateY(${-clockShift}px)`;
}
setInterval(() => {
  const now = new Date();
  if (now.getHours() * 60 + now.getMinutes() !== clockMinute) layoutClock();
}, 1000);
addEventListener('resize', layoutClock);

// обои и часы: показать или убрать за ms (0 — сразу). Часы проявляются, съезжая на место сверху на 4 % своей высоты
// за 180 мс, уходят вверх и тают за 140 — как у приставки (pi/dock/lock.cpp)
let wallOn = true, clockOn = true;
function setWall(on, ms) {
  wallOn = on;
  wallEl.style.transition = ms ? `opacity ${ms}ms ${EASE}` : 'none';
  wallEl.style.opacity = on ? 1 : 0;
  if (on) guestEl.style.display = 'none';
}
function setClock(on, ms) {
  clockOn = on;
  clockEl.style.transition = ms ? `opacity ${ms}ms ${EASE}, transform ${ms}ms ${EASE}` : 'none';
  clockEl.style.opacity = on && screenCfg.clock ? 1 : 0;
  clockEl.style.transform = on ? 'none' : `translateY(${-clockShift}px)`;
}
const splash = (on, wallMs, clockMs) => { setWall(on, wallMs); setClock(on, clockMs); };
layoutClock();
splash(true, 0, 0);

// ---------- связь ----------

function cableUp() {
  if (CONNECTION.usbPort) return Promise.resolve(true);
  return new Promise((done) => {
    const s = net.connect({ host: CABLE, port: 22, timeout: 700 });
    s.on('connect', () => { s.destroy(); done(true); });
    s.on('error', () => done(false));
    s.on('timeout', () => { s.destroy(); done(false); });
  });
}

// Пути. Окно держит сразу два подключения к телефону — по кабелю и по Wi-Fi — с одним билетом сеанса (Join), и всё
// идёт по обоим: каждый кадр телефон шлёт по всем путям (здесь берётся копия, пришедшая первой, повтор — по метке
// захвата — выбрасывается), ввод отсюда — по всем живым путям в Input с номером (телефон выполняет один раз). Обрыв
// пути ничего не прерывает: следующий кадр уже идёт по другому, в том же потоке, декодер его просто продолжает
// (docs/STREAM.md, «Кабель или Wi-Fi»). Основной путь (active, сообщение Use) — для значка и цифр. Каждые 0,1 с (keepRoute):
//  • кабель вынут (у Mac пропал адрес сети кабеля 10.77.0.x) — его путь закрыть, основной — Wi-Fi;
//  • кабель вставлен (адрес появился, телефон по нему отвечает) — открыть путь по кабелю и, как заговорил, основной — он;
//  • основной молчит (пинг — каждые 0,1 с; по кабелю ответ — за 1–2 мс, тишина 0,3 с — обрыв; по Wi-Fi бывают
//    всплески до 0,1 с, там — 0,6 с), а другой живой — основной он. macOS убирает адрес вынутого кабеля не сразу
//    (≈ 0,8 с), но кадры и ввод всё это время и так шли по обоим путям.
// По чему идёт на самом деле, говорит телефон (Route: интерфейс, на который пришло подключение, — по его ядру), — это и в
// заголовке окна, и значком на панели стола.
const TOKEN = crypto.randomBytes(8);     // билет сеанса: у окна свой
const paths = new Map();                 // 'кабель' | 'wi-fi' → подключение (TCP или процесс ssh)
const failures = new Map();
let active = null;                       // путь видео и ввода
const retryAt = { 'кабель': 0, 'wi-fi': 0 };
let probing = false;
let off = false;                         // телефон сказал: трансляция выключена (Switch 0) — ждём на одном тихом пути
let wantedCodec = IS_WINDOWS ? 'h264' : 'hevc';

function wifiHost() {
  if (CONNECTION.via === 'usb') return null;
  if (CONNECTION.host) return CONNECTION.host;
  try {
    const text = fs.readFileSync(path.join(ROOT, 'private', 'device.json'), 'utf8').replace(/^\uFEFF/, '');
    const host = JSON.parse(text).wifi_ip;
    return typeof host === 'string' ? host : null;
  } catch { return null; }
}

function cableLink() {
  if (CONNECTION.usbPort) return true;   // порт ADB уже выделен launcher
  if (!CONNECTION.cableNetwork) return false;
  // Адрес интерфейса кабельной сети выдал телефон.
  return Object.values(os.networkInterfaces()).flat().some((a) => a && a.family === 'IPv4' && a.address.startsWith('10.77.0.'));
}

function open(way, use) {
  const host = way === 'кабель' ? CABLE : wifiHost();
  if (!host) return;
  const quiet = off;
  const direct = way === 'кабель' && CONNECTION.usbPort;
  const peer = direct ? net.createConnection({ host: '127.0.0.1', port: direct })
    : spawn('ssh', sshArguments(ROOT, host, CONNECTION.port, quiet ? PARKED_PING_MS / 1000 : 2, process.platform),
      { windowsHide: true });
  if (direct) peer.setNoDelay(true);
  const me = {
    direct, way, quiet, writer: direct ? peer : peer.stdin, close: () => direct ? peer.destroy() : peer.kill(),
    inbuf: Buffer.alloc(0), lastData: 0, ready: false, route: 0, rtt: 0, opened: Date.now(),
  };
  paths.set(way, me);
  const error = (text) => {
    const detail = (direct ? 'ADB: ' : 'SSH: ') + text.trim();
    console.warn(detail);
    if (IS_WINDOWS && !me.ready) failures.set(way, detail);
  };
  peer.on('error', (err) => error(err.message));
  if (!direct) {
    peer.stdin.on('error', () => {});   // путь оборвался, пока писали в трубу
    peer.stderr.on('data', (d) => error(d.toString()));
  }
  const reader = direct ? peer : peer.stdout;
  reader.on('data', (d) => {
    if (paths.get(way) !== me) return;
    me.lastData = Date.now();
    me.ready = true;
    failures.delete(way);
    try {
      const buffer = me.inbuf.length ? Buffer.concat([me.inbuf, d]) : d;
      me.inbuf = readMessages(buffer, (type, body) => {
        if (paths.get(way) === me) handle(me, type, body);
      });
    } catch (err) {
      console.warn('Трансляция:', err.message);
      me.close();
    }
  });
  peer.on('close', () => {
    if (paths.get(way) !== me) return;
    paths.delete(way);
    if (direct && !me.ready && !failures.has(way)) failures.set(way, 'ADB: служба трансляции не отвечает');
    retryAt[way] = Date.now() + 1500;
    if (active === me) {
      active = null;
      showVia();
    }
  });
  const join = Buffer.alloc(9);
  TOKEN.copy(join, 0);
  join[8] = use ? 1 : 0;
  sendOn(me, T.JOIN, join);
  sendOn(me, T.SCREENS, screensJson());   // кто мы и какой у нас экран — вторым словом каждого пути, и тихого
  if (use && !quiet) {
    setActive(me);
    wantVideo();
    syncDesk(true);   // форма стола — раньше выбора стола: телефон пришлёт один Hello уже нужного размера
    send(T.MODE, Buffer.from([mode === 'desktop' ? 1 : 0]));
  }
  pingOn(me);
}

// Экран этого окна для «Экранов» телефона (Screens), размер — окна в пикселях.
const SCREEN_ID = IS_WINDOWS ? 'win' : IS_MAC ? 'mac' : 'linux';
function screensJson() {
  return Buffer.from(JSON.stringify({
    viewer: SCREEN_ID,
    wall: wallHash,
    screens: [{ id: SCREEN_ID, name: PLATFORM_NAME, kind: 'window',
      w: Math.round(innerWidth * devicePixelRatio), h: Math.round(innerHeight * devicePixelRatio) }],
  }));
}
// частота стола, когда главный экран — это окно: частота экрана Mac, но не больше 60 (стол и кодер настроены под 60)
const SCREEN_HZ = Math.min(60, Math.max(30, Math.round(ipcRenderer.sendSync('screen-hz'))));
function wantVideo() {
  send(T.WANT, Buffer.from(JSON.stringify({ codec: wantedCodec, fps: SCREEN_HZ, lock: true })));
}

function setActive(me) {
  active = me;
  pongs = [];          // задержка — уже этого пути (сдвиг часов остаётся: часы телефона те же)
  showVia();
  console.log(`связь: ${me.way}`);
}

function use(me) {
  if (active === me) return;
  sendOn(me, T.USE);
  setActive(me);
}

// по чему идёт: со слов телефона (Route), пока он не сказал — как подключались
function currentVia() {
  if (!active) return '';
  if (active.direct) return 'кабель';
  return active.route === 1 ? 'кабель' : active.route === 2 ? 'wi-fi' : active.way;
}
let shownVia = null;
function showVia() {
  via = currentVia();
  if (via === shownVia) return;
  shownVia = via;
  ipcRenderer.send('via', via);
  checkAwdl();
}

// Стол на это окно не выведен (или выключен) либо выведен (Switch): все пути закрыть — откроются заново, тихим или
// обычными. Не выведен — заставка (стол ушёл с экрана сам — часы и обои уже проявились по меткам Away: это не меняется)
function setOff(on) {
  if (on === off && on) return;
  off = on;
  console.log(on ? 'телефон: стол сюда не выведен — заставка, жду тихо' : 'телефон: стол выведен сюда — подключаюсь');
  paths.forEach((p) => p.close());
  paths.clear();
  active = null;
  retryAt['кабель'] = retryAt['wi-fi'] = 0;
  showVia();
  status('');
  if (on) {
    clearTimeout(clockLater);
    entering = false;
    splash(true, wallOn ? 0 : 250, clockOn ? 0 : 180);
  }
}

// «Стол сюда» (Wake): щелчок или клавиша на заставке — по тихому пути; что это значит (включить, перенести, добавить),
// решает телефон (docs/SCREENS.md, «Пробуждение»)
let wakeAt = 0;
function wake() {
  if (!off) return false;
  if (Date.now() - wakeAt < 1000) return true;
  const p = [...paths.values()].find((x) => x.ready);
  if (!p) return true;
  wakeAt = Date.now();
  sendOn(p, T.WAKE, Buffer.from(SCREEN_ID));
  console.log('щелчок на заставке — прошу стол сюда');
  return true;
}

async function keepRoute() {
  const now = Date.now();
  const link = cableLink();
  const cable = paths.get('кабель');
  if (cable && !link) {   // кабель вынут — путь мёртв, ждать SSH (≈ 6 с) незачем
    paths.delete('кабель');
    cable.close();
    if (active === cable) active = null;
  }
  if (cable?.direct && !off && now - Math.max(cable.lastData, cable.opened) > (cable.ready ? 5000 : 1800))
    cable.close();
  if (off) {   // один тихий путь — по кабелю, если он есть, иначе по Wi-Fi; молчит дольше 95 с — мёртв
    lostSince = 0;
    paths.forEach((p) => { if (now - Math.max(p.lastData, p.opened) > (p.direct && !p.ready ? 1800 : PARKED_SILENT_MS)) p.close(); });
    if (paths.size || probing) return;
    const useCable = link && (!CONNECTION.usbPort || now >= retryAt['кабель'] || !wifiHost());
    const way = useCable ? 'кабель' : 'wi-fi';
    if (now >= retryAt[way] && (way === 'кабель' || wifiHost())) open(way, false);
    return;
  }
  const alive = (p) => p && p.ready && now - p.lastData < (p.way === 'кабель' ? 300 : 600);
  // нет ни одного пути — первый, сразу с видео: по кабелю, если он есть и телефон по нему отвечает
  // телефон пропал (ни один путь не говорит 1,5 с) — обои и часы поверх застывшего стола, как у приставки
  if ([...paths.values()].some((p) => p.ready && now - p.lastData < 1500)) lostSince = 0;
  else if (!lostSince) lostSince = now;
  else if (now - lostSince > 1500 && !wallOn) splash(true, 500, 180);
  if (!paths.size) {
    const failure = failures.get('wi-fi') || failures.get('кабель');
    status(failure || (shown ? 'нет связи — переподключаюсь' : 'подключаюсь…'));
    if (probing) return;
    if (link && now >= retryAt['кабель']) {
      probing = true;
      const ok = await cableUp();
      probing = false;
      if (paths.size) return;
      if (ok) return open('кабель', true);
      retryAt['кабель'] = Date.now() + 1500;
    }
    if (now >= retryAt['wi-fi'] && wifiHost()) open('wi-fi', true);
    else if (!link && !wifiHost()) status(IS_WINDOWS ? 'Для подключения запустите tools/mirror_win.py' : 'телефона не видно');
    return;
  }
  // запасные — когда первый заговорил (сеанс начат)
  if (active && active.ready) {
    if (link && !paths.has('кабель') && now >= retryAt['кабель'] && !probing) {
      probing = true;
      const ok = await cableUp();
      probing = false;
      if (ok && cableLink() && !paths.has('кабель')) open('кабель', false);
      else retryAt['кабель'] = Date.now() + 1500;
    }
    if (CONNECTION.via !== 'wifi' && !paths.has('wi-fi') && now >= retryAt['wi-fi'] && wifiHost()) open('wi-fi', false);
  }
  // видео — по кабелю, когда он живой; иначе — по живому
  const c = paths.get('кабель'), w = paths.get('wi-fi');
  const best = alive(c) ? c : alive(w) ? w : null;
  if (best && best !== active && (best === c || !alive(active))) use(best);
  else if (!active) {
    const any = [c, w].find((p) => p && p.ready);
    if (any) use(any);
  }
}
window.addEventListener('beforeunload', () => paths.forEach((p) => p.close()));

// AWDL Mac (AirDrop, Handoff, AirPlay, Universal Control) раз в 0,52 с уводит Wi-Fi Mac с канала на ~90 мс —
// по Wi-Fi это провалы связи (docs/STREAM.md, «Плавность по Wi-Fi»); в цифрах (⌘I) — подсказка
let awdl = false;
function checkAwdl() {
  awdl = false;
  if (!IS_MAC || via !== 'wi-fi') return;
  execFile('/sbin/ifconfig', ['awdl0'], (err, out) => { awdl = !err && /status: active/.test(out); });
}

function sendOn(proc, type, body = Buffer.alloc(0)) {
  if (proc?.writer.writable) proc.writer.write(message(type, body));
}

// ввод — по всем путям, что сказали хоть слово, с номером: телефон выполнит один раз, по какому бы ни пришёл
let inputSeq = 0;
function send(type, body = Buffer.alloc(0)) {
  if (off) return;   // трансляция выключена: телефон ввода не ждёт
  const inner = input(++inputSeq, type, body);
  paths.forEach((p) => { if (p.ready || p === active) sendOn(p, T.INPUT, inner); });
}

function pingOn(proc) {
  const b = Buffer.alloc(8);
  b.writeBigUInt64LE(BigInt(nowUs()));
  sendOn(proc, T.PING, b);
}

let fitted = null;
function handle(me, type, body) {
  if (type === T.PONG) {
    const mac = Number(body.readBigUInt64LE(0)), phone = Number(body.readBigUInt64LE(8)), back = nowUs();
    me.rtt = back - mac;
    if (me !== active) return;
    pongs.push({ rtt: back - mac, offset: phone - (mac + back) / 2 });
    pongs = pongs.slice(-20);
    const best = pongs.reduce((a, b) => (b.rtt < a.rtt ? b : a));
    offsetUs = best.offset;
    stats.rtt = best.rtt;
  } else if (type === T.SWITCH) {
    setOff(body[0] === 0);
  } else if (type === T.ROUTE) {
    me.route = body[0];
    if (me === active) showVia();
  } else if (type === T.CURSOR) {
    canvas.style.cursor = desktop() ? CURSOR_CSS[body[0]] || 'default' : 'default';
  } else if (type === T.CURSORAT) {   // мышь стола ведёт другой экран (приставка): стрелка там, где она
    const r = frameRect();
    guestEl.style.display = body[0] && !wallOn ? 'block' : 'none';
    guestEl.style.transform = `translate(${r.left + body.readFloatLE(1) * r.width - 3}px, ${r.top + body.readFloatLE(5) * r.height - 2}px)`;
  } else if (type === T.SCREEN) {   // настройки экрана стола: нам — часы заставки (есть ли, где, ширина стола)
    try {
      const c = JSON.parse(body.toString());
      screenCfg = { clock: c.clock ?? 1, clockX: c.clockX ?? 275, clockY: c.clockY ?? 378, width: c.width || 1600 };
      localStorage.setItem('screenCfg', JSON.stringify(screenCfg));
      layoutClock();
      setClock(clockOn, 0);
    } catch {}
  } else if (type === T.WALLPAPER) {   // обои стола — заставке: помнятся до следующего запуска
    const hash = body.subarray(0, 16).toString().replace(/\0+$/, ''), jpeg = body.subarray(16);
    if (hash !== wallHash) {
      wallHash = hash;
      applyWall(jpeg);
      try {
        if (jpeg.length) fs.writeFileSync(WALL_FILE, jpeg);
        localStorage.setItem('wall', hash);
      } catch {}
      console.log(`обои заставки: ${jpeg.length ? Math.round(jpeg.length / 1024) + ' КБ' : 'нет (градиент)'}`);
    }
  } else if (type === T.AWAY) {   // метка кадра: стол уходит с экрана или возвращается — до её Video
    awayMarks.set(Number(body.readBigUInt64LE(0)), body[8]);
    if (awayMarks.size > 16) awayMarks.delete(awayMarks.keys().next().value);
  } else if (type === T.HELLO && off) {   // ждали тихо, а телефон ведёт сеанс (включили, пока он перезапускался)
    setOff(false);
  } else if (type === T.HELLO) {   // новый поток (новый сеанс или сменили, что смотрим): декодер — заново
    const h = JSON.parse(body.toString());
    if (h.gen && hello && h.gen <= hello.gen && h.gen > hello.gen - 1000) return;   // копия по другому пути
    hello = h;
    // холст — нового размера с первым кадром нового потока (draw): иначе на смене формы стола он на миг пустел
    resetDecoder();
    status('');
    entering = true;   // первый кадр потока — на экран: заставка (если она есть) тает, телефону — Reveal
    // окно — под то, что смотрим, только когда это сменилось (не при переходе кабель ↔ Wi-Fi: размер — как был)
    const mode = hello.mode || 'phone';
    const fit = IS_WINDOWS && mode === 'phone' ? [mode, hello.width, hello.height].join(':') : mode;
    if (fit !== fitted) {
      fitted = fit;
      ipcRenderer.send('fit', mode, { width: hello.width, height: hello.height });
    } else if (hello.mode === 'desktop') {
      syncDesk(false);   // стол не той формы, что окно (телефон перезапускали) — поправить
    }
  } else if (type === T.VIDEO) {
    // кадр — по всем путям: первая копия, вторая (метка захвата не новее) — мимо
    me.videos = (me.videos || 0) + 1;
    const capture = Number(body.readBigUInt64LE(0));
    if (capture > lastCapture || lastCapture - capture >= 10e6) me.first = (me.first || 0) + 1;
    // (часы телефона пошли заново — перезагрузился — метка «в прошлом» на много секунд: это не копия)
    if (capture <= lastCapture && lastCapture - capture < 10e6) return;
    lastCapture = capture;
    onVideo(body);
  }
}
let lastCapture = 0;

// ---------- видео ----------

let decoderRetry = null, decodeFailed = false;

function resetDecoder() {
  if (decoder && decoder.state !== 'closed') decoder.close();
  decoder = null;
  clearTimeout(decoderRetry);
  decoderRetry = null;
  decodeFailed = false;
  waitingKey = true;
  pending.clear();
  while (queue.length) queue.shift().frame.close();
  lastVideo = null;
}

function decoderError(error, codec) {
  console.error('декодер:', error?.name || '', error?.message || error);
  resetDecoder();
  if (!IS_WINDOWS) {
    send(T.KEYFRAME);
    return;
  }
  decodeFailed = true;
  if (isHevc(codec) && wantedCodec === 'hevc') {
    wantedCodec = 'h264';
    status('HEVC недоступен. Запрашиваю H.264…');
    wantVideo();
  } else {
    status(isHevc(codec) ? 'HEVC недоступен. Кодек общий для всех экранов'
      : 'Ошибка декодирования H.264. Запрашиваю опорный кадр…');
  }
  decoderRetry = setTimeout(() => {
    decoderRetry = null;
    send(T.KEYFRAME);
  }, 750);
}

function configure(codec) {
  const current = new VideoDecoder({
    output: onFrame,
    error: (error) => { if (decoder === current) decoderError(error, codec); },
  });
  decoder = current;
  // H.264 на Windows может декодироваться программно, если профиль недоступен с ускорением.
  const acceleration = IS_WINDOWS && !isHevc(codec) ? 'no-preference' : 'prefer-hardware';
  decoder.configure(decoderConfig(codec, IS_WINDOWS ? hello : null, acceleration));
}

// NAOMI_MIRROR_DUMP=файл.h264 — принятый поток (первые копии кадров), как есть: разобрать ffmpeg (качество кадров)
const DUMP = process.env.NAOMI_MIRROR_DUMP;
if (DUMP) fs.writeFileSync(DUMP, Buffer.alloc(0));
let lastVideo = null;
function onVideo(body) {
  if (DUMP) fs.appendFileSync(DUMP, body.subarray(17));
  const capture = Number(body.readBigUInt64LE(0));
  const encoded = Number(body.readBigUInt64LE(8));
  const key = body[16] & 1;
  const data = body.subarray(17);
  stats.bytes += data.length;
  if (waitingKey && (!key || decoderRetry)) return;
  try {
    if (!decoder) configure(annexBCodec(data, hello?.codec));
    waitingKey = false;
    const recv = nowUs();
    // провал связи: кадр пришёл позже, чем был снят после прошлого (в движении — кадры идут подряд)
    if (lastVideo && capture - lastVideo.capture < 50000)
      stats.stall = Math.max(stats.stall, (recv - lastVideo.recv - (capture - lastVideo.capture)) / 1000);
    lastVideo = { capture, recv };
    pending.set(capture, { recv, encoded });
    if (pending.size > 30) pending.delete(pending.keys().next().value);
    decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: capture, data }));
  } catch (error) {
    decoderError(error, annexBCodec(data, hello?.codec));
  }
}

// NAOMI_MIRROR_TRACE=файл.csv — времена каждого кадра (для замеров плавности): захват и сжатие — часы телефона,
// приход, распаковка и показ — часы Mac (показ 0 — кадр выброшен, не показан), сдвиг часов; мкс
const TRACE = process.env.NAOMI_MIRROR_TRACE;
if (TRACE) fs.writeFileSync(TRACE, 'capture,encoded,recv,decoded,drawn,offset\n');
function trace(item, drawn) {
  if (TRACE && item.info)
    fs.appendFileSync(TRACE, `${item.frame.timestamp},${item.info.encoded},${item.info.recv},${item.decoded},${drawn},${offsetUs}\n`);
}

// ---------- показ: по развёртке экрана ----------
// Кадр рисуется в обновление экрана Mac (requestAnimationFrame), каждый — в своё. Было: рисовался, как только
// распакован, и если два кадра пришли между обновлениями, первый не был виден вовсе (по Wi-Fi — до 27% кадров,
// замер 13 ноября 2026). Задержки это не прибавляет: нарисованное и раньше попадало на экран только со
// следующим обновлением. Связь замерла, а потом кадры пришли пачкой, — догоняем по кадру за обновление, но не
// больше чем на LAG кадров: старшие выбрасываются, задержка не копится. Экран обновляется реже, чем идут кадры
// (60 Гц при 90 к/с), — всегда самый свежий.
const LAG = 3;
const queue = [];          // распакованные, ждут своего обновления экрана
let ticking = false, lastTick = 0, tickMs = 8.3;
function onFrame(frame) {
  const info = pending.get(frame.timestamp);
  pending.delete(frame.timestamp);
  queue.push({ frame, info, decoded: nowUs() });
  const keep = tickMs > 1000 / ((hello && hello.fps) || 90) + 1 ? 1 : LAG;
  while (queue.length > keep) {
    const old = queue.shift();
    trace(old, 0);
    old.frame.close();
    stats.skipped++;
  }
  if (!ticking) {
    ticking = true;
    requestAnimationFrame(tick);
  }
}

function tick(t) {
  if (lastTick && t - lastTick < 50) tickMs += (t - lastTick - tickMs) * 0.1;   // частота экрана — средним
  lastTick = t;
  const item = queue.shift();
  if (item) draw(item);
  if (queue.length || item) requestAnimationFrame(tick);
  else { ticking = false; lastTick = 0; }
}

function draw(item) {
  const { frame, info } = item;
  if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
    canvas.width = frame.displayWidth;
    canvas.height = frame.displayHeight;
  }
  ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
  const drawn = nowUs();
  trace(item, drawn);
  if (info && offsetUs !== null) {
    const captured = frame.timestamp - offsetUs;   // захват кадра — по часам Mac
    stats.add('phone', (info.encoded - frame.timestamp) / 1000);
    stats.add('net', (info.recv - info.encoded + offsetUs) / 1000);
    stats.add('decode', (item.decoded - info.recv) / 1000);
    stats.add('total', (drawn - captured) / 1000);
  }
  const mark = awayMarks.get(frame.timestamp);
  awayMarks.delete(frame.timestamp);
  frame.close();
  stats.frames++;
  if (!shown || decodeFailed) {
    shown = true;
    decodeFailed = false;
    status('');
  }
  if (entering) {
    // Первый кадр потока на экране. Стол начинался для нас (одни обои — те же, что на заставке): обои прочь, телефону
    // Reveal — панель и окна проявятся, а часы уйдут в тот же кадр (метка AwayBack). Стол уже шёл (смотрят другие
    // экраны) — метки не будет: часы уходят сами
    entering = false;
    send(T.REVEAL);
    if (wallOn || clockOn) {
      setWall(false, 200);
      clearTimeout(clockLater);
      clockLater = setTimeout(() => setClock(false, 140), 350);
    }
  }
  if (mark === 0) {            // с этого кадра панель и окна уходят — часы проявляются
    clearTimeout(clockLater);
    setClock(true, 180);
  } else if (mark === 1) {     // одни обои на экране: дальше — заставка; телефону можно менять план
    setWall(true, 200);
    send(T.AWAYSHOWN);
  } else if (mark === 2) {     // стол возвращается — часы уходят
    clearTimeout(clockLater);
    setClock(false, 140);
    if (wallOn) setWall(false, 200);
  }
}

// ---------- цифры (⌘I) ----------

const stats = {
  frames: 0, skipped: 0, stall: 0, bytes: 0, rtt: 0, series: {},
  add(name, v) { (this.series[name] = this.series[name] || []).push(v); },
};
const median = (v) => (v && v.length ? [...v].sort((a, b) => a - b)[Math.floor(v.length / 2)] : NaN);
const f1 = (v) => (Number.isFinite(v) ? v.toFixed(1) : '—');
let statFrom = performance.now();
setInterval(() => {
  const sec = (performance.now() - statFrom) / 1000;
  const s = stats.series;
  const spare = [...paths.values()].find((p) => p !== active && p.ready && Date.now() - p.lastData < 600);
  // кадров по каждому пути за секунду: пришло / из них первыми
  const per = [...paths.values()].map((p) => `${p.way} ${p.videos || 0}/${p.first || 0}`).join(', ');
  paths.forEach((p) => { p.videos = 0; p.first = 0; });
  const line = `${desktop() ? 'стол' : IS_WINDOWS ? 'планшет' : 'телефон'} ${hello ? `${hello.width}×${hello.height}` : '…'} · ${via}`
    + `${spare ? ` (запасной: ${spare.way}, пинг ${f1(spare.rtt / 1000)} мс)` : ''} · ${Math.round(stats.frames / sec)} к/с · `
    + `${f1(stats.bytes * 8 / sec / 1e6)} Мбит/с\n`
    + `телефон (кадр → сжат) ${f1(median(s.phone))} мс\n`
    + `связь ${f1(median(s.net))} мс · провал до ${f1(stats.stall)} мс · пинг ${f1(stats.rtt / 1000)} мс\n`
    + `распаковка ${f1(median(s.decode))} мс\n`
    + `итого до ${PLATFORM_NAME} ${f1(median(s.total))} мс · не показано ${stats.skipped}\n`
    + `кадры по путям (пришло / первыми): ${per}`
    + (awdl ? '\nAWDL Mac включён: провалы связи ~90 мс раз в 0,5 с' : '');
  hudEl.textContent = line;
  if (stats.frames && document.body.classList.contains('hud')) console.log(line.replace(/\n/g, ' | '));
  stats.frames = 0;
  stats.skipped = 0;
  stats.stall = 0;
  stats.bytes = 0;
  stats.series = {};
  statFrom = performance.now();
}, 1000);

// ---------- ввод ----------

// где на холсте сам кадр: он вписан в окно со своими пропорциями (object-fit: contain)
function frameRect() {
  const r = canvas.getBoundingClientRect();
  const k = Math.min(r.width / canvas.width, r.height / canvas.height);
  const w = canvas.width * k, h = canvas.height * k;
  return { left: r.left + (r.width - w) / 2, top: r.top + (r.height - h) / 2, width: w, height: h };
}

function norm(e) {
  const r = frameRect();
  return [(e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height];
}

function touch(phase, x, y) {
  const b = Buffer.alloc(10);
  b[0] = phase;
  b[1] = 0;
  b.writeFloatLE(x, 2);
  b.writeFloatLE(y, 6);
  send(T.TOUCH, b);
}

// Рабочий стол — мышь как мышь: каждое движение (и без кнопок — наведение), кнопки (1 — левая, 2 — правая,
// 4 — средняя). Экран телефона — мышь как палец: нажал — коснулся, повёл, отпустил.
let buttons = 0;
function pointer(e) {
  const [x, y] = norm(e);
  const b = Buffer.alloc(9);
  // и Shift, Ctrl (⌘), Alt — выделение с Ctrl и Shift в «Файлах»
  b[0] = buttons | (e.shiftKey ? 16 : 0) | (e.ctrlKey || (IS_MAC && e.metaKey) ? 32 : 0) | (e.altKey ? 64 : 0);
  b.writeFloatLE(x, 1);
  b.writeFloatLE(y, 5);
  send(T.POINTER, b);
}

let touchId = null;
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('pointerdown', (e) => {
  if (e.pointerType === 'touch' && !e.isPrimary) return;
  if (wake()) return;   // заставка: щелчок — «стол сюда»
  if (desktop()) {
    canvas.setPointerCapture(e.pointerId);
    buttons = e.buttons & 7;
    pointer(e);
    return;
  }
  if (e.button !== 0 || touchId !== null) return;
  canvas.setPointerCapture(e.pointerId);
  touchId = e.pointerId;
  touch(0, ...norm(e));
});
canvas.addEventListener('pointermove', (e) => {
  if (e.pointerType === 'touch' && !e.isPrimary) return;
  guestEl.style.display = 'none';   // своя мышь в окне — чужая стрелка не нужна
  // все точки между кадрами Chromium — скорость пальца и ход мыши точнее
  const all = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
  if (desktop()) {
    buttons = e.buttons & 7;
    for (const p of all.length ? all : [e]) pointer(p);
    return;
  }
  if (touchId !== e.pointerId) return;
  for (const p of all.length ? all : [e]) touch(1, ...norm(p));
});
const lift = (phase) => (e) => {
  if (e.pointerType === 'touch' && !e.isPrimary) return;
  if (desktop()) {
    if (phase === 3 && !buttons) return;
    buttons = phase === 3 ? 0 : e.buttons & 7;
    pointer(e);
    return;
  }
  if (touchId !== e.pointerId) return;
  touchId = null;
  touch(phase, ...norm(e));
};
canvas.addEventListener('pointerup', lift(2));
canvas.addEventListener('pointercancel', lift(3));
canvas.addEventListener('lostpointercapture', lift(3));

// прокрутка: сдвиг в долях экрана, как у колеса в браузере (вниз — положительный); конец — после паузы
let wheelEnd = null, wheelAt = [0.5, 0.5];
function scroll(x, y, dx, dy, phase) {
  const b = Buffer.alloc(17);
  b.writeFloatLE(x, 0);
  b.writeFloatLE(y, 4);
  b.writeFloatLE(dx, 8);
  b.writeFloatLE(dy, 12);
  b[16] = phase;
  send(T.SCROLL, b);
}
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  if (e.ctrlKey) return;   // щипок трекпада — не прокрутка
  const r = frameRect();
  const units = e.deltaMode === 1 ? [16, 16] : e.deltaMode === 2 ? [r.width, r.height] : [1, 1];
  if (!wheelEnd) wheelAt = norm(e);
  scroll(wheelAt[0], wheelAt[1], (e.deltaX * units[0]) / r.width, (e.deltaY * units[1]) / r.height, 1);
  clearTimeout(wheelEnd);
  wheelEnd = setTimeout(() => {
    wheelEnd = null;
    scroll(wheelAt[0], wheelAt[1], 0, 0, 2);
  }, 90);
}, { passive: false });

// клавиши Qt (Qt::Key) и модификаторы; ⌘ на Mac — Ctrl у Qt (выделить всё, отменить)
const KEY_V = 0x56, KEY_H = 0x48, ALT_BACKSPACE = [0x01000003, 0x08000000];
const mods = (e) => (e.shiftKey ? SHIFT : 0) | (e.ctrlKey || e.metaKey ? CTRL : 0) | (e.altKey ? ALT : 0);

function key(down, qtKey, modifiers, text) {
  const t = Buffer.from(text || '', 'utf8');
  const b = Buffer.alloc(9 + t.length);
  b[0] = down ? 1 : 0;
  b.writeUInt32LE(qtKey >>> 0, 1);
  b.writeUInt32LE(modifiers >>> 0, 5);
  t.copy(b, 9);
  send(T.KEY, b);
}
const nav = (action) => send(T.NAV, Buffer.from([action]));

function paste() {
  // Стол получает Ctrl+V вместе с текстом, экран устройства — сам текст.
  const pasteMode = mode;
  ipcRenderer.invoke('clipboard-text').then((text) => {
    if (mode !== pasteMode || off) return;
    if (desktop()) { key(true, KEY_V, CTRL, text); key(false, KEY_V, CTRL, text); }
    else if (text) { key(true, 0, 0, text); key(false, 0, 0, text); }
  }).catch((error) => console.warn('Буфер обмена:', error));
}

const DESK_CMD = {
  Backspace: () => ALT_BACKSPACE,
  ArrowUp: () => [QT_KEY.ArrowUp, ALT],
  ArrowDown: () => [QT_KEY.Enter, 0],
  BracketLeft: () => [QT_KEY.ArrowLeft, ALT],
  BracketRight: () => [QT_KEY.ArrowRight, ALT],
  Period: (e) => (e.shiftKey ? [KEY_H, CTRL] : [0, 0]),
};

const shotKeys = new Set();   // ⇧⌥3 / ⇧⌥4, ушедшие столу: их отпускание — тоже клавишей
function onMacKey(e, down) {
  if (off) {   // заставка: клавиша (не ⌘-сочетание окна и не один модификатор) — «стол сюда»
    if (down && !e.metaKey && !['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(e.key)) {
      e.preventDefault();
      wake();
    }
    return;
  }
  // скриншот стола (docs/DESKTOP.md, «Скриншоты»): ⇧⌥3 — весь стол, ⇧⌥4 — область, как Shift+Alt+3/4 в НаомиОС.
  // Цифрой по месту клавиши, а не символом раскладки (⌥⇧3 у Mac — «‹»); ⇧⌘3/4 Mac забирает себе
  if (desktop() && (e.code === 'Digit3' || e.code === 'Digit4') &&
      (down ? e.altKey && e.shiftKey && !e.metaKey && !e.ctrlKey : shotKeys.has(e.code))) {
    e.preventDefault();
    if (down) shotKeys.add(e.code); else shotKeys.delete(e.code);
    key(down, e.code.charCodeAt(5), SHIFT | ALT, '');
    return;
  }
  // столу Ctrl+буква или цифра (настоящий Ctrl Mac) — сочетанием, как ⌘ (а не буквой)
  if (e.ctrlKey && !e.metaKey && desktop() && /^(Key[A-Z]|Digit[0-9])$/.test(e.code)) {
    e.preventDefault();
    key(down, e.code.charCodeAt(e.code.length - 1), CTRL | (e.shiftKey ? SHIFT : 0), '');
    return;
  }
  if (e.metaKey) {
    // свои сочетания; ⌘Q, ⌘W, ⌘R — меню окна; ⌘1 / ⌘2 — и в меню «Вид» (повтор безвреден: телефон тот же выбор
    // не переключает)
    if (['KeyQ', 'KeyW', 'KeyR', 'KeyH', 'KeyM'].includes(e.code)) return;
    e.preventDefault();
    if (!down) return;
    if (e.code === 'Digit1') setMode('desktop');
    else if (e.code === 'Digit2') setMode('phone');
    else if (e.code === 'ArrowUp' && !desktop()) nav(NAV.HOME);
    else if (e.code === 'ArrowDown' && !desktop()) nav(NAV.SHADE);
    else if (e.code === 'KeyI') document.body.classList.toggle('hud');
    else if (e.code === 'KeyV') paste();
    else if (desktop() && DESK_CMD[e.code]) {
      // сочетания Finder — клавишами «Файлов» НаомиОС: ⌘⌫ в корзину, ⌘↑ наверх, ⌘↓ открыть, ⌘[ ⌘] назад/вперёд,
      // ⌘⇧. скрытые
      const [k, m] = DESK_CMD[e.code](e);
      if (k) { key(true, k, m, ''); key(false, k, m, ''); }
    } else if (/^Key[A-Z]$/.test(e.code)) {
      const k = e.code.charCodeAt(3);   // латинская буква клавиши — при любой раскладке
      key(true, k, CTRL | (e.shiftKey ? SHIFT : 0), '');
      key(false, k, CTRL | (e.shiftKey ? SHIFT : 0), '');
    }
    return;
  }
  e.preventDefault();
  if (e.key === 'Escape') {   // телефону — «назад», столу — клавиша Esc
    if (desktop()) key(down, QT_KEY.Escape, mods(e), '');
    else if (down) nav(NAV.BACK);
    return;
  }
  if (QT_KEY[e.key]) key(down, QT_KEY[e.key], mods(e), e.key === 'Enter' ? '\r' : e.key === 'Tab' ? '\t' : '');
  else if (e.key.length === 1 || [...e.key].length === 1) key(down, 0, 0, e.key);   // буква с раскладки Mac
}

const heldKeys = new Map();   // отпускание с тем же кодом и модификаторами, с которыми нажали
const windowActions = {
  desktop: () => setMode('desktop'),
  phone: () => setMode('phone'),
  hud: () => document.body.classList.toggle('hud'),
};
const deviceActions = { paste, back: () => nav(NAV.BACK), home: () => nav(NAV.HOME), shade: () => nav(NAV.SHADE) };
function onDesktopKey(e, down) {
  if (heldKeys.has(e.code)) {
    e.preventDefault();
    const args = heldKeys.get(e.code);
    if (!down) heldKeys.delete(e.code);
    if (args) key(down, ...args);
    return;
  }
  if (!down || e.metaKey || e.isComposing) return;
  const action = shortcut(e, desktop());
  const run = windowActions[action] || (!off && deviceActions[action]);
  if (run) {
    e.preventDefault();
    heldKeys.set(e.code, null);
    run();
    return;
  }
  if (off) {
    if (!MODIFIER_KEYS.has(e.key)) {
      e.preventDefault();
      wake();
    }
    return;
  }
  const args = remoteKey(e);
  if (!args) return;
  e.preventDefault();
  heldKeys.set(e.code, args);
  key(true, ...args);
}

window.addEventListener('blur', () => {
  heldKeys.forEach((args) => { if (args) key(false, ...args); });
  heldKeys.clear();
});
const onKey = IS_MAC ? onMacKey : onDesktopKey;
window.addEventListener('keydown', (e) => onKey(e, true));
window.addEventListener('keyup', (e) => onKey(e, false));

if (process.env.NAOMI_MIRROR_HUD) document.body.classList.add('hud');
async function start() {
  if (IS_WINDOWS) wantedCodec = await windowsCodec(globalThis.VideoDecoder);
  keepRoute();
  setInterval(keepRoute, 100);
  setInterval(() => paths.forEach((p) => { if (!p.quiet) pingOn(p); }), 100);
  setInterval(() => paths.forEach((p) => { if (p.quiet) pingOn(p); }), PARKED_PING_MS);
}
start();
