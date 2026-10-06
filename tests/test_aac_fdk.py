# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the FDK AAC decoder path and the GET /info formats bitmask.

Builds tests/host/aac_harness.cpp against the same decoder files and the same
pinned FDK source the board uses, then feeds it ffmpeg-encoded clips with a
distinct tone on every channel. AirPlay strips ADTS, so the harness rebuilds
each header from the RTP SSRC exactly as the firmware does.

Run: uv run tests/test_aac_fdk.py   (or uv run --with pytest pytest tests/)
"""
import array
import math
import plistlib
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "airplay_receiver"
CACHE = Path(__file__).resolve().parent / ".cache"
FDK_REPO = "https://github.com/pschatzmann/codec-fdk-aac.git"
FDK_REF = "9998bb9e5fe839ddccb0aeba8ff8c5afc1c94ed1"  # keep in step with __init__.py
FDK_DIRS = ["libAACdec", "libFDK", "libSYS", "libArithCoding", "libMpegTPDec",
            "libSACdec", "libDRCdec", "libSBRdec", "libPCMutils"]

SSRC_44100_STEREO = "16000000"
SSRC_48000_STEREO = "17000000"
SSRC_48000_5POINT1 = "27000000"
SSRC_48000_7POINT1 = "28000000"
FORMAT_AAC_44100_STEREO = 1 << 22
FORMAT_AAC_48000_5POINT1 = 1 << 39

# One tone per input channel, in ffmpeg's channel order for the layout. The
# downmix should put front-left / surround-left content in L only, the right
# side in R only, and centre in both.
TONES_5POINT1 = {"FL": 300, "FR": 500, "FC": 700, "LFE": 60, "BL": 1100, "BR": 1300}
TONES_7POINT1 = {"FL": 300, "FR": 500, "FC": 700, "LFE": 60, "BL": 1100, "BR": 1300,
                 "SL": 1700, "SR": 1900}
TONES_STEREO = {"FL": 440, "FR": 880}
DECODE_TASK_STACK = 57344  # AUDIO_DECODE_TASK_STACK in audio_decode_worker.cpp
DECODE_STACK_MARGIN = 4096  # the task's own frames on top of the decoder's
CLIP_SECONDS = 2
ANALYSIS_FRAMES = 8192
PRESENT_DB = -30.0   # tone counts as present above this, relative to the loudest
ABSENT_DB = -50.0    # and as absent below this

pytestmark = pytest.mark.skipif(
    shutil.which("ffmpeg") is None or shutil.which("c++") is None or shutil.which("git") is None,
    reason="needs ffmpeg, a C++ compiler and git")


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        pytest.fail(f"{Path(cmd[0]).name} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result


@pytest.fixture(scope="session")
def harness() -> Path:
    fdk = CACHE / f"codec-fdk-aac-{FDK_REF[:12]}"
    if not fdk.exists():
        _run(["git", "init", "-q", str(fdk)])
        _run(["git", "-C", str(fdk), "fetch", "-q", "--depth", "1", FDK_REPO, FDK_REF])
        _run(["git", "-C", str(fdk), "checkout", "-q", "FETCH_HEAD"])
    library = CACHE / f"libfdk-{FDK_REF[:12]}.a"
    includes = [f"-I{fdk / 'src'}"] + [f"-I{fdk / 'src' / d}" for d in FDK_DIRS]
    if not library.exists():
        objects_dir = CACHE / f"obj-{FDK_REF[:12]}"
        objects_dir.mkdir(parents=True, exist_ok=True)
        sources = [p for d in FDK_DIRS for p in (fdk / "src" / d).glob("*.cpp")]
        sources += list((fdk / "src").glob("*.cpp"))
        objects = []
        for source in sources:
            obj = objects_dir / f"{source.parent.name}_{source.stem}.o"
            if not obj.exists():
                _run(["c++", "-std=c++17", "-O2", "-w", "-DUSE_DEFAULT_STDLIB", *includes,
                      "-c", str(source), "-o", str(obj)])
            objects.append(str(obj))
        _run(["ar", "rcs", str(library), *objects])
    binary = CACHE / "aac_harness"
    sources = [ROOT / "tests" / "host" / "aac_harness.cpp", COMPONENT / "decoder" / "aac_fdk.cpp",
               COMPONENT / "decoder" / "aac_format.cpp", COMPONENT / "transport" / "bplist.cpp"]
    _run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{COMPONENT}", *includes,
          *map(str, sources), str(library), "-lpthread", "-o", str(binary)])
    return binary


def _encoders() -> list[str]:
    """ffmpeg's encoder, plus Apple's AudioToolbox one where available (macOS)."""
    listing = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    return ["aac"] + (["aac_at"] if re.search(r"\baac_at\b", listing) else [])


def _encode(tmp: Path, name: str, tones: dict[str, int], rate: int, layout: str,
            encoder: str = "aac") -> Path:
    inputs, labels, channel_map = [], [], []
    for index, (channel, freq) in enumerate(tones.items()):
        inputs += ["-f", "lavfi", "-i", f"sine=frequency={freq}:sample_rate={rate}:duration={CLIP_SECONDS}"]
        labels.append(f"[{index}:a]")
        channel_map.append(f"{index}.0-{channel}")
    # An explicit map: without one, join routes each mono (FC) input to FC first.
    merge = (f"{''.join(labels)}join=inputs={len(tones)}:channel_layout={layout}"
             f":map={'|'.join(channel_map)}[out]")
    out = tmp / f"{name}.adts"
    _run(["ffmpeg", "-y", "-loglevel", "error", *inputs, "-filter_complex", merge, "-map", "[out]",
          "-c:a", encoder, "-b:a", "384k", "-f", "adts", str(out)])
    return out


def _decode(harness: Path, adts: Path, ssrc: str) -> tuple[dict[str, int], list[int], list[int]]:
    pcm_path = adts.with_suffix(".pcm")
    result = _run([str(harness), "decode", str(adts), ssrc, str(pcm_path)])
    stats = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", result.stdout)}
    samples = array.array("h", pcm_path.read_bytes())
    return stats, list(samples[0::2]), list(samples[1::2])


def _level_db(signal: list[int], freq: float, rate: int) -> float:
    """Goertzel magnitude of `freq` over a window from the middle of the clip."""
    start = len(signal) // 2 - ANALYSIS_FRAMES // 2
    window = signal[start:start + ANALYSIS_FRAMES]
    coeff = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s_prev = s_prev2 = 0.0
    last = len(window) - 1
    for index, sample in enumerate(window):
        hann = 0.5 - 0.5 * math.cos(2.0 * math.pi * index / last)  # keeps neighbouring tones out
        s = sample * hann + coeff * s_prev - s_prev2
        s_prev2, s_prev = s_prev, s
    power = s_prev2 ** 2 + s_prev ** 2 - coeff * s_prev * s_prev2
    return 10.0 * math.log10(max(power, 1e-9))


def _assert_downmix(left: list[int], right: list[int], tones: dict[str, int], rate: int,
                    left_only: set[str], right_only: set[str], both: set[str]) -> None:
    raw = {(side, name): _level_db(signal, freq, rate)
           for side, signal in (("L", left), ("R", right)) for name, freq in tones.items()}
    peak = max(raw.values())
    levels = {f"{side}.{name}": round(db - peak, 1) for (side, name), db in raw.items()}
    present = lambda side, name: levels[f"{side}.{name}"] > PRESENT_DB  # noqa: E731
    absent = lambda side, name: levels[f"{side}.{name}"] < ABSENT_DB  # noqa: E731
    for name in left_only:
        assert present("L", name) and absent("R", name), (name, levels)
    for name in right_only:
        assert present("R", name) and absent("L", name), (name, levels)
    for name in both:
        assert present("L", name) and present("R", name), (name, levels)


@pytest.mark.parametrize("encoder", _encoders() if shutil.which("ffmpeg") else ["aac"])
def test_5point1_downmixes_to_stereo(harness: Path, tmp_path: Path, encoder: str) -> None:
    adts = _encode(tmp_path, "five_one", TONES_5POINT1, 48000, "5.1", encoder)
    stats, left, right = _decode(harness, adts, SSRC_48000_5POINT1)
    assert stats["header_mismatches"] == 0
    assert stats["source_channels"] == 6 and stats["sample_rate"] == 48000
    assert stats["frames"] >= CLIP_SECONDS * 48000 // 1024
    _assert_downmix(left, right, TONES_5POINT1, 48000, {"FL", "BL"}, {"FR", "BR"}, {"FC"})


def test_7point1_decodes_every_channel(harness: Path, tmp_path: Path) -> None:
    """7.1 decodes and every channel reaches the mix.

    Where each pair lands is not asserted: channelConfiguration 7 means
    "front-wide" pairs to FDK and side pairs to ffmpeg's encoder, and Apple's
    AudioToolbox sidesteps it with an in-band PCE. Only a real sender settles
    which reading AirPlay 7.1 needs.
    """
    adts = _encode(tmp_path, "seven_one", TONES_7POINT1, 48000, "7.1")
    stats, left, right = _decode(harness, adts, SSRC_48000_7POINT1)
    assert stats["header_mismatches"] == 0
    assert stats["source_channels"] == 8
    _assert_downmix(left, right, {k: v for k, v in TONES_7POINT1.items() if k != "LFE"}, 48000,
                    set(), set(), {"FC"})
    for name, freq in TONES_7POINT1.items():
        if name != "LFE":
            loudest = max(_level_db(left, freq, 48000), _level_db(right, freq, 48000))
            assert loudest > _level_db(left, TONES_7POINT1["FC"], 48000) + PRESENT_DB, name


@pytest.mark.parametrize(("rate", "ssrc"), [(44100, SSRC_44100_STEREO), (48000, SSRC_48000_STEREO)])
def test_stereo_passes_through(harness: Path, tmp_path: Path, rate: int, ssrc: str) -> None:
    adts = _encode(tmp_path, f"stereo_{rate}", TONES_STEREO, rate, "stereo")
    stats, left, right = _decode(harness, adts, ssrc)
    assert stats["header_mismatches"] == 0
    assert stats["source_channels"] == 2 and stats["sample_rate"] == rate
    _assert_downmix(left, right, TONES_STEREO, rate, {"FL"}, {"FR"}, set())


def test_decode_stack_fits_task(harness: Path, tmp_path: Path) -> None:
    """Report the peak stack so AUDIO_DECODE_TASK_STACK can be sized from it."""
    adts = _encode(tmp_path, "stack", TONES_7POINT1, 48000, "7.1")
    stats, _, _ = _decode(harness, adts, SSRC_48000_7POINT1)
    print(f"peak decode stack (host): {stats['stack']} bytes")
    assert stats["stack"] < DECODE_TASK_STACK - DECODE_STACK_MARGIN


def test_info_default_has_no_supported_formats(harness: Path, tmp_path: Path) -> None:
    out = tmp_path / "info.plist"
    _run([str(harness), "info", "0", str(out)])
    info = plistlib.loads(out.read_bytes())
    assert "supportedFormats" not in info
    assert info["audioFormats"][0]["audioOutputFormats"] == 0x01000000


def test_info_advertises_buffer_stream_formats(harness: Path, tmp_path: Path) -> None:
    formats = FORMAT_AAC_44100_STEREO | FORMAT_AAC_48000_5POINT1
    out = tmp_path / "info.plist"
    _run([str(harness), "info", f"{formats:x}", str(out)])
    info = plistlib.loads(out.read_bytes())
    assert info["supportedFormats"] == {"bufferStream": formats}
    assert info["name"] == "Test"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v", "-s"]))
