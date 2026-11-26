# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Host tests for the pair role text form (audio/group_spec.cpp).

Home Assistant sets the group role through this text; a parser that accepts
garbage would reboot a speaker into a broken pair.

Run: uv run tests/test_group_spec.py   (or uv run --with pytest pytest tests/)
"""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
from host_stubs import write_stubs  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = next(c for c in (ROOT / "components" / "airplay_receiver", ROOT / "airplay_receiver")
                 if c.is_dir())
HOST = Path(__file__).resolve().parent / "host"
CACHE = Path(__file__).resolve().parent / ".cache"

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


def test_group_spec_harness() -> None:
    CACHE.mkdir(exist_ok=True)
    stubs = CACHE / "group_spec_stubs"
    write_stubs(stubs)
    binary = CACHE / "group_spec_harness"
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
         f"-I{COMPONENT}", f"-I{stubs}", str(HOST / "group_spec_harness.cpp"),
         str(COMPONENT / "audio" / "group_spec.cpp"), "-o", str(binary)],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip() == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
