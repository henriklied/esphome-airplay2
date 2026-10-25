# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the AAC framing the firmware rebuilds, and the GET /info plist.

AirPlay strips ADTS from buffered AAC and names the format in each packet's RTP
SSRC instead. The firmware rebuilds every header from the SSRC before handing
the frame to esp_audio_codec; these tests check the rebuilt headers against
the ones Apple's and ffmpeg's encoders write. The decoder itself only builds
for the ESP32, so it is verified on the board (tools/aac_bench at 9933c7b).

Run: uv run tests/test_aac_format.py   (or uv run --with pytest pytest tests/)
"""
import plistlib
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
# Standalone repo nests the component under components/; the monorepo copy does not.
COMPONENT = next(c for c in (ROOT / "components" / "airplay_receiver", ROOT / "airplay_receiver")
                 if c.is_dir())
CACHE = Path(__file__).resolve().parent / ".cache"

SSRC_44100_STEREO = "16000000"
SSRC_48000_STEREO = "17000000"
FORMAT_AAC_44100_STEREO = 1 << 22
FORMAT_AAC_48000_STEREO = 1 << 23
CLIP_SECONDS = 2

pytestmark = pytest.mark.skipif(
    shutil.which("ffmpeg") is None or shutil.which("c++") is None,
    reason="needs ffmpeg and a C++ compiler")


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        pytest.fail(f"{Path(cmd[0]).name} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result


@pytest.fixture(scope="session")
def harness() -> Path:
    CACHE.mkdir(exist_ok=True)
    binary = CACHE / "aac_format_harness"
    sources = [ROOT / "tests" / "host" / "aac_format_harness.cpp", COMPONENT / "decoder" / "aac_format.cpp",
               COMPONENT / "transport" / "bplist.cpp"]
    _run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{COMPONENT}", *map(str, sources),
          "-o", str(binary)])
    return binary


def _encoders() -> list[str]:
    """ffmpeg's encoder, plus Apple's AudioToolbox one where available (macOS)."""
    listing = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    return ["aac"] + (["aac_at"] if re.search(r"\baac_at\b", listing) else [])


def _encode_stereo(tmp: Path, rate: int, encoder: str) -> Path:
    out = tmp / f"stereo_{rate}_{encoder}.adts"
    _run(["ffmpeg", "-y", "-loglevel", "error",
          "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate={rate}:duration={CLIP_SECONDS}",
          "-f", "lavfi", "-i", f"sine=frequency=880:sample_rate={rate}:duration={CLIP_SECONDS}",
          "-filter_complex", "[0:a][1:a]join=inputs=2:channel_layout=stereo[out]", "-map", "[out]",
          "-c:a", encoder, "-b:a", "256k", "-f", "adts", str(out)])
    return out


@pytest.mark.parametrize("encoder", _encoders() if shutil.which("ffmpeg") else ["aac"])
@pytest.mark.parametrize(("rate", "ssrc"), [(44100, SSRC_44100_STEREO), (48000, SSRC_48000_STEREO)])
def test_rebuilt_headers_match_encoder(harness: Path, tmp_path: Path, rate: int, ssrc: str,
                                       encoder: str) -> None:
    adts = _encode_stereo(tmp_path, rate, encoder)
    stats = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)",
                                              _run([str(harness), "headers", str(adts), ssrc]).stdout)}
    assert stats["frames"] >= CLIP_SECONDS * rate // 1024
    assert stats["header_mismatches"] == 0


def test_surround_ssrc_is_not_a_format(harness: Path, tmp_path: Path) -> None:
    """5.1 is parked (tag parked/surround-5.1); its SSRC must not pass as stereo."""
    adts = _encode_stereo(tmp_path, 48000, "aac")
    result = subprocess.run([str(harness), "headers", str(adts), "27000000"], capture_output=True, text=True)
    assert result.returncode != 0


def test_info_default_has_no_supported_formats(harness: Path, tmp_path: Path) -> None:
    out = tmp_path / "info.plist"
    _run([str(harness), "info", "0", str(out)])
    info = plistlib.loads(out.read_bytes())
    assert "supportedFormats" not in info
    assert info["audioFormats"][0]["audioOutputFormats"] == 0x01000000


def test_info_advertises_buffer_stream_formats(harness: Path, tmp_path: Path) -> None:
    formats = FORMAT_AAC_44100_STEREO | FORMAT_AAC_48000_STEREO
    out = tmp_path / "info.plist"
    _run([str(harness), "info", f"{formats:x}", str(out)])
    info = plistlib.loads(out.read_bytes())
    assert info["supportedFormats"] == {"bufferStream": formats}
    assert info["name"] == "Test"


def test_info_advertises_audio_stream_formats(harness: Path, tmp_path: Path) -> None:
    buffer_formats, audio_formats = FORMAT_AAC_44100_STEREO, 0x1440000
    out = tmp_path / "info.plist"
    _run([str(harness), "info", f"{buffer_formats:x}", f"{audio_formats:x}", str(out)])
    info = plistlib.loads(out.read_bytes())
    assert info["supportedFormats"] == {"bufferStream": buffer_formats, "audioStream": audio_formats}
    assert info["name"] == "Test"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v", "-s"]))
