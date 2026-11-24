// НаомиФон Mirror — окно с рабочим столом телефона или его экраном (docs/STREAM.md, «Окно на Mac»; docs/DESKTOP.md).
// Electron: renderer.js ведёт трансляцию и ввод, здесь — окно и меню. На Mac AWDL выключен, пока окно открыто.
//   tools/mirror        — запустить из репозитория (Electron — mirror/node_modules: npm install)
//   tools/mirror_win.py — запуск через ADB или Wi-Fi (docs/WINDOWS-MIRROR.md)
//   mirror/build.sh     — NaomiPhone Mirror.app со своим значком, в /Applications
// Для проверок: NAOMI_MIRROR_SHOT=файл.png — снимок окна раз в 2 с; NAOMI_MIRROR_HUD=1 — цифры сразу и в журнал
// (ELECTRON_ENABLE_LOGGING=1); NAOMI_MIRROR_INPUT=файл.json — сценарий ввода: окно подаёт в страницу события
// ([{wait: мс, event: {...}}], как webContents.sendInputEvent) каждый раз, когда файл меняется, — тот же путь, что
// у мыши и клавиатуры; ещё — размер окна и полный экран (windowSize, fullscreen: форма стола). NAOMI_MIRROR_TRACE=файл.csv — времена каждого кадра (renderer.js, «Плавность по Wi-Fi»);
// NAOMI_MIRROR_DUMP=файл.h264 — принятый поток (разбор качества: ffmpeg -debug qp, docs/STREAM.md, «Качество»).
const { app, BrowserWindow, Menu, clipboard, dialog, ipcMain, nativeImage, screen } = require('electron');
const fs = require('fs');
const path = require('path');
const { execFile, execFileSync } = require('child_process');
const { accelerators } = require('./keyboard');
const IS_MAC = process.platform === 'darwin';
const IS_WINDOWS = process.platform === 'win32';
const SHORTCUTS = accelerators(IS_MAC);

// AWDL (docs/STREAM.md, «Плавность по Wi-Fi»): радио AirDrop, Handoff, AirPlay раз в полсекунды уводит Wi-Fi Mac с
// канала роутера на 90–190 мс — главная причина рывков трансляции по Wi-Fi. Пока окно открыто, AWDL выключен, и раз
// в 2 с — проверка: macOS включает его сама (просят AirDrop, Handoff…) — снова выключить. Закрыли окно — включить
// обратно: AirDrop и Handoff снова работают. Права — одно разрешение sudo без пароля ровно на эти две команды,
// пользователь ставит его сам (/etc/sudoers.d/naomiphone-awdl, там же в STREAM.md); без него окно работает как раньше.
const awdl = {
  allowed: IS_MAC,
  set(up, sync) {
    if (!this.allowed) return;
    const args = ['-n', '/sbin/ifconfig', 'awdl0', up ? 'up' : 'down'];
    if (sync) {
      try { execFileSync('/usr/bin/sudo', args); } catch {}
      return;
    }
    execFile('/usr/bin/sudo', args, (e) => {
      if (!e) return;
      this.allowed = false;   // разрешения нет — больше не пробовать
      console.log('AWDL: нет разрешения sudo (docs/STREAM.md) — остаётся как есть');
    });
  },
  check() {
    execFile('/sbin/ifconfig', ['awdl0'], (e, out) => {
      if (!e && /status: active/.test(out)) {
        console.log('AWDL: включился — выключаю');
        this.set(false);
      }
    });
  },
  start() {
    if (!this.allowed) return;
    this.check();
    setInterval(() => this.check(), 2000);
  },
};

// Окно — то, что смотрим: рабочий стол (docs/DESKTOP.md) — 1280 × 720 точек, точка в точку со столом; экран телефона
// 720 × 1560 — 360 × 780 точек (на Retina с двойной плотностью — пиксель в пиксель). Не помещается (у Mac пользователя
// экран «как 1352 × 878», с Dock — 793 точки вместе с заголовком) — меньше, в тех же пропорциях.
const CHROME = IS_WINDOWS ? 72 : 32;   // место под рамку, заголовок и меню Windows
const VIEW = { desktop: { w: 1280, h: 720 }, phone: { w: 360, h: 780 } };

function fitSize(mode, frame) {
  // Размер и ориентация встроенного экрана pipa приходят в Hello.
  const v = IS_WINDOWS && mode === 'phone' && frame?.width > 0 && frame?.height > 0
    ? { w: frame.width / 2, h: frame.height / 2 } : VIEW[mode] || VIEW.desktop;
  const area = screen.getPrimaryDisplay().workAreaSize;
  const k = Math.min(1, (area.width - 16) / v.w, (area.height - CHROME - 8) / v.h);
  return { width: Math.round(v.w * k), height: Math.round(v.h * k) };
}

function windowIcon() {
  const png = path.join(__dirname, 'icon.png');
  if (!IS_WINDOWS) return png;
  const ico = nativeImage.createFromPath(path.join(__dirname, 'icon.ico'));
  return ico.isEmpty() ? png : ico;
}

function createWindow() {
  const { width, height } = fitSize('desktop');
  const win = new BrowserWindow({
    width,
    height,
    useContentSize: true,
    minWidth: 180,
    minHeight: 180,
    title: IS_WINDOWS ? 'NaomiPhone Mirror' : 'NaomiPhone',
    backgroundColor: '#000000',
    ...(IS_WINDOWS ? { icon: windowIcon(), autoHideMenuBar: false } : {}),
    webPreferences: {
      // страница своя, из репозитория: ей нужен Node (ssh, файлы ключей)
      nodeIntegration: true,
      contextIsolation: false,
      backgroundThrottling: false,
    },
  });
  if (IS_WINDOWS) win.on('page-title-updated', (event) => event.preventDefault());
  win.loadFile(path.join(__dirname, 'index.html'));
  // телефон сказал, что теперь идёт (Hello): окно — под него
  // заголовок: что смотрим и по чему идёт (кабель или Wi-Fi — окно переходит само, docs/STREAM.md)
  let shown = 'desktop', way = '';
  const name = IS_WINDOWS ? 'NaomiPhone Mirror' : 'NaomiPhone';
  const title = () => win.setTitle(name + (shown === 'phone' ? '' : ' — рабочий стол')
    + (way === 'кабель' ? ' · по кабелю' : way ? ' · по Wi-Fi' : ''));
  ipcMain.on('fit', (_e, mode, frame) => {
    const { width: w, height: h } = fitSize(mode, frame);
    win.setContentSize(w, h);
    shown = mode;
    title();
  });
  ipcMain.on('via', (_e, v) => {
    way = v;
    title();
  });
  return win;
}

// Текст локального буфера — странице (у неё модуля clipboard нет).
ipcMain.handle('clipboard-text', () => clipboard.readText());
// где хранить обои заставки (docs/SCREENS.md) и какая частота у экрана — странице
ipcMain.on('user-data', (e) => { e.returnValue = app.getPath('userData'); });
ipcMain.on('screen-hz', (e) => { e.returnValue = screen.getPrimaryDisplay().displayFrequency || 60; });

function setMenu(win) {
  const show = (mode) => win.webContents.send('mode', mode);
  const view = {
    label: 'Вид',
    submenu: [
      { label: 'Рабочий стол', accelerator: SHORTCUTS.desktop, registerAccelerator: IS_MAC, click: () => show('desktop') },
      { label: IS_WINDOWS ? 'Экран планшета' : 'Экран телефона', accelerator: SHORTCUTS.phone,
        registerAccelerator: IS_MAC, click: () => show('phone') },
      { type: 'separator' },
    ],
  };
  if (IS_MAC) {
    view.submenu.push({ role: 'reload', label: 'Переподключить' }, { role: 'toggleDevTools' });
    Menu.setApplicationMenu(Menu.buildFromTemplate([{ role: 'appMenu' }, view, { role: 'windowMenu' }]));
    return;
  }
  // Сочетания обрабатывает renderer, который знает режим экрана и различает AltGr и Ctrl+Alt.
  view.submenu.push(
    { label: 'Статистика', accelerator: SHORTCUTS.hud, registerAccelerator: false,
      click: () => win.webContents.send('toggle-hud') },
    { label: 'Переподключить', click: () => win.webContents.reload() },
    { label: 'Инструменты разработчика', click: () => win.webContents.toggleDevTools() },
  );
  const help = {
    label: 'Справка',
    submenu: [{
      label: 'Сочетания клавиш',
      click: () => dialog.showMessageBox(win, {
        title: 'NaomiPhone Mirror',
        message: 'Сочетания клавиш',
        detail: [
          SHORTCUTS.desktop + ' / ' + SHORTCUTS.phone + ' — рабочий стол / экран устройства',
          SHORTCUTS.hud + ' — статистика кадра и связи',
          'Ctrl+V — вставить текст из локального буфера',
          'Ctrl+C/A/Z/X — команды рабочего стола',
          'Esc / Alt+↑ / Alt+↓ — назад / домой / шторка на экране устройства',
          'Alt+↑ / Alt+← / Alt+→ / Alt+Backspace / Enter / Ctrl+H — команды «Файлов»',
          'Shift+Alt+3 / Shift+Alt+4 — снимок рабочего стола / области',
        ].join('\n'),
      }),
    }],
  };
  Menu.setApplicationMenu(Menu.buildFromTemplate([view, help]));
}

let mainWin = null;
if (IS_WINDOWS && !app.requestSingleInstanceLock()) {
  app.quit();
} else {
  if (IS_WINDOWS) {
    app.on('second-instance', () => {
      if (!mainWin || mainWin.isDestroyed()) return;
      if (mainWin.isMinimized()) mainWin.restore();
      mainWin.show();
      mainWin.focus();
    });
  }

  app.whenReady().then(() => {
    if (IS_WINDOWS) app.setAppUserModelId('com.popka.naomiphone.mirror');
    awdl.start();
    const win = createWindow();
    mainWin = win;
    win.on('closed', () => { if (mainWin === win) mainWin = null; });
    setMenu(win);
    const shot = process.env.NAOMI_MIRROR_SHOT;
    if (shot)
      setInterval(() => win.webContents.capturePage().then((img) => fs.writeFileSync(shot, img.toPNG())), 2000);
    const script = process.env.NAOMI_MIRROR_INPUT;
    let played = '';
    if (script)
      setInterval(() => {
        let text = '';
        try { text = fs.readFileSync(script, 'utf8'); } catch { return; }
        if (!text || text === played) return;
        played = text;
        let at = 0;
        for (const step of JSON.parse(text)) {
          at += step.wait || 0;
          // {type: 'windowSize', width, height} — размер окна (проверка формы стола), {type: 'fullscreen', on}
          const e = step.event;
          setTimeout(() => e.type === 'windowSize' ? win.setContentSize(e.width, e.height)
            : e.type === 'fullscreen' ? win.setFullScreen(!!e.on) : win.webContents.sendInputEvent(e), at);
        }
      }, 200);
  });
}

app.on('window-all-closed', () => app.quit());
app.on('will-quit', () => awdl.set(true, true));
