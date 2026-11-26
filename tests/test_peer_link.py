# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the stereo-pair link (audio/peer_protocol.h, audio/peer_clock.h).

Builds tests/host/peer_harness.cpp: wire-format round trips and rejection of
corrupt datagrams, and the relayed clock against a simulated Wi-Fi link with
crystal drift, queueing jitter, stalls and a primary clock step.

Run: uv run tests/test_peer_link.py   (or uv run --with pytest pytest tests/)
"""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = next(c for c in (ROOT / "components" / "airplay_receiver", ROOT / "airplay_receiver")
                 if c.is_dir())
CACHE = Path(__file__).resolve().parent / ".cache"

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


def test_peer_harness() -> None:
    CACHE.mkdir(exist_ok=True)
    binary = CACHE / "peer_harness"
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=undefined", f"-I{COMPONENT}",
         str(Path(__file__).resolve().parent / "host" / "peer_harness.cpp"), "-o", str(binary)],
        check=False, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip().splitlines()[-1] == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v", "-s"]))
