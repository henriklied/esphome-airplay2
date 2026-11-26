# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the filter cascade's text form (audio/dsp_spec.cpp).

Home Assistant edits the EQ through a `text` entity holding this form, so a
parser that accepts garbage or loses precision would reach the speaker.

Run: uv run tests/test_dsp_spec.py   (or uv run --with pytest pytest tests/)
"""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = next(c for c in (ROOT / "components" / "airplay_receiver", ROOT / "airplay_receiver")
                 if c.is_dir())
HOST = Path(__file__).resolve().parent / "host"
CACHE = Path(__file__).resolve().parent / ".cache"

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


def test_dsp_spec_harness() -> None:
    CACHE.mkdir(exist_ok=True)
    binary = CACHE / "dsp_spec_harness"
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
         f"-I{COMPONENT}", f"-I{HOST / 'stub'}", str(HOST / "dsp_spec_harness.cpp"),
         str(COMPONENT / "audio" / "dsp_spec.cpp"), "-o", str(binary)],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip() == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
