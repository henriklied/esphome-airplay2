# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the dynamic DSP stages (audio/audio_dsp.cpp).

Builds tests/host/dsp_harness.cpp against a stub logger and checks the limiter
ceiling, latency and release, volume-following loudness, harmonic bass and
stereo width on synthetic signals.

Run: uv run tests/test_buffered_ring.py   (or uv run --with pytest pytest tests/)
"""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
# Standalone repo nests the component under components/; the monorepo copy does not.
COMPONENT = next(c for c in (ROOT / "components" / "airplay_receiver", ROOT / "airplay_receiver")
                 if c.is_dir())
HOST = Path(__file__).resolve().parent / "host"
CACHE = Path(__file__).resolve().parent / ".cache"

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


def test_dsp_harness() -> None:
    CACHE.mkdir(exist_ok=True)
    binary = CACHE / "dsp_harness"
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=undefined", f"-I{COMPONENT}", f"-I{HOST / 'stub'}",
         str(HOST / "dsp_harness.cpp"), str(COMPONENT / "audio" / "audio_dsp.cpp"), "-o", str(binary)],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip() == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
