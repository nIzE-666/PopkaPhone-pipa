const AVC_CODEC = 'avc1.42E033';
const HEVC_CODEC = 'hev1.1.6.L156.B0';   // Main, уровень 5.2 (docs/STREAM.md)
const isHevc = (codec) => /^(hvc1|hev1)(\.|$)/.test(codec || '');
const hex = (value) => value.toString(16).toUpperCase().padStart(2, '0');

function annexBCodec(data, advertised) {
  if (data) {
    // Определяем кодек по NAL в Annex B (3- или 4-байтовый стартовый код) раньше advertised:
    // после Want устройство может переключить кодер без повторного Hello.
    for (let i = 0; i + 4 < data.length; i++) {
      if (data[i] !== 0 || data[i + 1] !== 0 || data[i + 2] !== 1) continue;
      const b0 = data[i + 3];
      const b1 = data[i + 4];
      const hevcType = (b0 >> 1) & 0x3f;
      if ((b0 & 0x81) === 0 && (hevcType === 32 || hevcType === 33) && (b1 & 0x07) >= 1)
        return HEVC_CODEC;
      if ((b0 & 0x1f) === 7 && i + 6 < data.length)
        return 'avc1.' + hex(b1) + hex(data[i + 5]) + hex(data[i + 6]);
    }
  }
  if (isHevc(advertised)) return HEVC_CODEC;
  return advertised || AVC_CODEC;
}

function decoderConfig(codec, frame, hardwareAcceleration = 'prefer-hardware') {
  const config = {
    codec,
    optimizeForLatency: true,
    hardwareAcceleration,
    // Annex B: параметры кодека идут с опорным кадром. Цвет — как упакован в nv12_pack.cpp.
    colorSpace: { primaries: 'bt709', transfer: 'iec61966-2-1', matrix: 'bt709', fullRange: false },
  };
  if (frame) {
    config.codedWidth = frame.width;
    config.codedHeight = frame.height;
  }
  return config;
}

async function windowsCodec(VideoDecoder) {
  try {
    // Проверяем профиль и размер экрана pipa, который может прийти в режиме «Экран телефона».
    const { supported } = await VideoDecoder.isConfigSupported(decoderConfig(HEVC_CODEC, { width: 2880, height: 1800 }));
    if (supported) return 'hevc';
  } catch {}
  return 'h264';
}

module.exports = { AVC_CODEC, HEVC_CODEC, isHevc, annexBCodec, decoderConfig, windowsCodec };
