# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Compile the production timeline and mailbox diagnostics against host stubs."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "airplay_receiver"
HOST = ROOT / "tests" / "host"

sys.path.insert(0, str(HOST))
from host_stubs import write_stubs

pytestmark = pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")


@pytest.mark.parametrize(
    ("harness", "sources"),
    [("realtime_audio_harness", ("audio_timeline", "audio_plc")),
     ("udp_diagnostics_harness", ("audio_udp_diagnostics",))],
)
def test_realtime_audio(tmp_path: Path, harness: str, sources: tuple[str, ...]) -> None:
    write_stubs(tmp_path)
    binary = tmp_path / harness
    build = subprocess.run(
        ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=undefined", f"-I{tmp_path}", f"-I{HOST / 'stub'}",
         f"-I{COMPONENT}", str(HOST / f"{harness}.cpp"),
         *(str(COMPONENT / "audio" / f"{source}.cpp") for source in sources),
         "-o", str(binary)],
        capture_output=True, text=True, check=False,
    )
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15, check=False)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip() == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
