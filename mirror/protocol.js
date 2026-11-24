// TCP и SSH передают один и тот же протокол из shell/stream/stream_protocol.h.
const MAX_MESSAGE = 32 * 1024 * 1024;   // вместе с байтом типа, с запасом для опорных кадров

function message(type, body) {
  const packet = Buffer.alloc(5 + body.length);
  packet.writeUInt32LE(1 + body.length, 0);
  packet[4] = type;
  body.copy(packet, 5);
  return packet;
}

function input(sequence, type, body) {
  const packet = Buffer.alloc(5 + body.length);
  packet.writeUInt32LE(sequence, 0);
  packet[4] = type;
  body.copy(packet, 5);
  return packet;
}

function readMessages(buffer, handle) {
  let at = 0;
  while (buffer.length - at >= 4) {
    const size = buffer.readUInt32LE(at);
    if (size < 1 || size > MAX_MESSAGE) throw new RangeError('Неверная длина сообщения трансляции');
    const end = at + 4 + size;
    if (end > buffer.length) break;
    handle(buffer[at + 4], buffer.subarray(at + 5, end));
    at = end;
  }
  return buffer.subarray(at);
}

module.exports = { message, input, readMessages };
