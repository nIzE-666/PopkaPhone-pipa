const path = require('path');

const STREAM_PORT = 8478;

function tcpPort(value) {
  const port = Number(value);
  return /^\d+$/.test(value) && port > 0 && port <= 65535 ? port : null;
}

function connectionOptions(env, platform) {
  const via = env.NAOMI_MIRROR_VIA || 'auto';
  return {
    via,
    host: env.NAOMI_MIRROR_HOST,
    port: tcpPort(env.NAOMI_MIRROR_PORT) || STREAM_PORT,
    usbPort: via === 'wifi' ? null : tcpPort(env.NAOMI_MIRROR_USB_PORT),
    cableNetwork: platform !== 'win32' && via !== 'wifi' && via !== 'usb',
  };
}

function sshArguments(root, host, port, keepAlive, platform) {
  const ssh = path.join(root, 'private', 'ssh');
  const known = path.join(ssh, 'known_hosts');
  const win = platform === 'win32';
  // Значение -o отдельно разбирается OpenSSH. Кавычки сохраняют пробелы пути Windows.
  const knownOption = win ? '"' + known.replaceAll('\\', '/') + '"' : known;
  return [
    '-i', path.join(ssh, 'naomiphone_ed25519'),
    '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4',
    '-o', 'ServerAliveInterval=' + keepAlive, '-o', 'ServerAliveCountMax=3',
    '-o', 'UserKnownHostsFile=' + knownOption, '-o', 'HostKeyAlias=naomiphone',
    ...(win ? ['-o', 'StrictHostKeyChecking=yes'] : []),
    '-W', '127.0.0.1:' + port, 'phablet@' + host,
  ];
}

module.exports = { connectionOptions, sshArguments };
