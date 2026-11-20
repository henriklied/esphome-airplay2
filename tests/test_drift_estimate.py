# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the playout drift estimate (audio/drift_estimate.h).

Builds tests/host/drift_harness.cpp, which checks steady drift and that a
PTP timescale step neither freezes nor spikes the estimate.

Run: uv run tests/test_drift_estimate.py   (or uv run --with pytest pytest tests/)
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
CACHE = Path(__file__).resolve().parent / ".cache"

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


def test_drift_harness() -> None:
    CACHE.mkdir(exist_ok=True)
    binary = CACHE / "drift_harness"
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=undefined", f"-I{COMPONENT}",
         str(Path(__file__).resolve().parent / "host" / "drift_harness.cpp"), "-o", str(binary)],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip() == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
