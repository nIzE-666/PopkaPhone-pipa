import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STREAM_SERVICE = ROOT / "shell/stream/stream_service.cpp"


class StreamWantHelloTest(unittest.TestCase):
    def test_want_sends_hello_before_encoder_starts(self):
        source = STREAM_SERVICE.read_text(encoding="utf-8")
        match = re.search(
            r"void StreamService::Worker::want\(bool hevc, int fps, int encodeFps\)\s*\{(?P<body>.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "StreamService::Worker::want() not found")
        body = match.group("body")

        self.assertIn("const bool announce = hevc != m_hevc || fps != m_deskFps;", body)
        self.assertIn("if (m_enc.queues().inputs)", body)
        self.assertRegex(
            body,
            r"else\s+if\s*\(\s*announce\s*&&\s*m_session\s*\)\s*\{\s*hello\(\);\s*\}",
            "want() must send hello() when codec/fps changes in an active session before encoder queues exist",
        )

    def test_hello_announces_hevc_codec_when_enabled(self):
        source = STREAM_SERVICE.read_text(encoding="utf-8")
        match = re.search(
            r"void StreamService::Worker::hello\(int path\)\s*\{(?P<body>.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "StreamService::Worker::hello() not found")
        body = match.group("body")
        self.assertIn('{"codec", m_hevc ? "hvc1" : "avc1.42E033"}', body)


if __name__ == "__main__":
    unittest.main()
