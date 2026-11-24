const assert = require('node:assert/strict');
const path = require('node:path');
const { test } = require('node:test');
const { connectionOptions, sshArguments } = require('../connection');
const { message, input, readMessages } = require('../protocol');
const { annexBCodec, decoderConfig, HEVC_CODEC, windowsCodec } = require('../video');

test('ADB and Wi-Fi use the same framing and input sequence numbers', () => {
  const frame = message(21, input(0x01020304, 15, Buffer.from([2])));
  assert.deepEqual(frame, Buffer.from([7, 0, 0, 0, 21, 4, 3, 2, 1, 15, 2]));
  const pong = message(3, Buffer.from([0, 1]));
  const stream = Buffer.concat([frame, pong]);
  const seen = [];
  const handle = (type, body) => seen.push([type, body]);
  let remainder = Buffer.alloc(0);
  for (const chunk of [stream.subarray(0, 2), stream.subarray(2, 8), stream.subarray(8)]) {
    remainder = readMessages(Buffer.concat([remainder, chunk]), handle);
  }
  assert.equal(remainder.length, 0);
  assert.deepEqual(seen, [[21, frame.subarray(5)], [3, pong.subarray(5)]]);
  assert.throws(() => readMessages(Buffer.alloc(4), handle), RangeError);
  assert.throws(() => readMessages(Buffer.alloc(4, 0xff), handle), RangeError);
});

test('Windows uses the forwarded loopback port and leaves SSH on the device stream port', () => {
  const env = { NAOMI_MIRROR_USB_PORT: '39411', NAOMI_MIRROR_HOST: '192.0.2.42', NAOMI_MIRROR_PORT: '8479' };
  assert.deepEqual(connectionOptions(env, 'win32'), {
    via: 'auto', host: '192.0.2.42', usbPort: 39411, port: 8479, cableNetwork: false,
  });
  assert.equal(connectionOptions({ ...env, NAOMI_MIRROR_VIA: 'wifi' }, 'win32').usbPort, null);
  assert.equal(connectionOptions({ NAOMI_MIRROR_PORT: '99999' }, 'win32').port, 8478);
  assert.equal(connectionOptions({}, 'darwin').cableNetwork, true);
  assert.equal(connectionOptions({}, 'linux').cableNetwork, true);
  const root = path.join(path.resolve('.'), 'repo with spaces');
  const argv = sshArguments(root, env.NAOMI_MIRROR_HOST, 8479, 2, 'win32');
  assert.deepEqual(argv.slice(-3), ['-W', '127.0.0.1:8479', 'phablet@192.0.2.42']);
  assert.ok(argv.includes('StrictHostKeyChecking=yes'));
  assert.ok(argv.includes('UserKnownHostsFile="' + path.join(root, 'private', 'ssh', 'known_hosts').replaceAll('\\', '/') + '"'));
});

test('Annex B detects HEVC VPS/SPS and H.264 SPS over stale advertised codecs', () => {
  const hevcVpsSps = Buffer.from(
    '0000000140010c01ffff016000000300b0000003000003009cac09'
    + '00000001420101016000000300b0000003000003009ca00a080c81'
    + '000000014401c0f2f03c90',
    'hex',
  );
  assert.equal(annexBCodec(hevcVpsSps, 'avc1.42E033'), HEVC_CODEC);
  assert.equal(annexBCodec(hevcVpsSps), HEVC_CODEC);
  for (const start of ['000001', '00000001']) {
    assert.equal(annexBCodec(Buffer.from(start + '40010c01', 'hex'), 'avc1.42E033'), HEVC_CODEC);
    assert.equal(annexBCodec(Buffer.from(start + '42010101', 'hex'), 'avc1.42E033'), HEVC_CODEC);
    assert.equal(annexBCodec(Buffer.from(start + '6742c033de', 'hex')), 'avc1.42C033');
    assert.equal(annexBCodec(Buffer.from(start + '6742c033de', 'hex'), 'hvc1'), 'avc1.42C033');
    assert.equal(annexBCodec(Buffer.from(start + '6742c033de', 'hex'), HEVC_CODEC), 'avc1.42C033');
  }
  assert.equal(annexBCodec(Buffer.from('0000000140000c01', 'hex'), 'avc1.42E033'), 'avc1.42E033');
  assert.equal(annexBCodec(Buffer.alloc(0)), 'avc1.42E033');
  assert.equal(annexBCodec(Buffer.alloc(0), 'hvc1'), HEVC_CODEC);
  assert.equal(annexBCodec(Buffer.alloc(0), 'avc1.640028'), 'avc1.640028');
  const config = decoderConfig(HEVC_CODEC, { width: 2880, height: 1800 });
  assert.equal(config.hardwareAcceleration, 'prefer-hardware');
  assert.equal(config.codedWidth, 2880);
  assert.equal(config.codedHeight, 1800);
  assert.equal(config.description, undefined);
});

test('Windows asks for HEVC only when WebCodecs supports the tablet frame', async () => {
  assert.equal(await windowsCodec({ isConfigSupported: async (cfg) => ({ supported: cfg.codec === HEVC_CODEC }) }), 'hevc');
  assert.equal(await windowsCodec({ isConfigSupported: async () => ({ supported: false }) }), 'h264');
  assert.equal(await windowsCodec(undefined), 'h264');
});
