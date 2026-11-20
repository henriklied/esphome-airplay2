# /// script
# requires-python = ">=3.11"
# ///
"""Render a song with realtime packet loss, concealed two ways, for an A/B listen.

Holes of 2, 6, 17, 28 and 40 packets (the burst sizes measured on Geneva),
one every 4 s. Writes plc.wav (current concealment) and silence.wav (the
previous fade-to-silence) to the output directory.

Run: uv run tools/plc_demo/run.py <song> [<out-dir>]
Any format afconvert reads works; it is converted to 44.1 kHz 16-bit stereo.
"""

import logging
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "components" / "airplay_receiver"
HOST = ROOT / "tests" / "host"
sys.path.insert(0, str(HOST))
from host_stubs import write_stubs  # noqa: E402

logger = logging.getLogger("plc_demo")


def main() -> int:
    """Convert, build and render; return a process exit status."""
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    if len(sys.argv) not in (2, 3):
        logger.error(__doc__)
        return 2
    song = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) == 3 else Path.cwd()
    out_dir.mkdir(parents=True, exist_ok=True)
    if shutil.which("afconvert") is None or shutil.which("c++") is None:
        logger.error("needs afconvert (macOS) and a C++ compiler")
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        wav = work / "in.wav"
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@44100", "-c", "2",
                        str(song), str(wav)], check=True)
        write_stubs(work)
        binary = work / "plc_demo"
        subprocess.run(
            ["c++", "-std=c++17", "-O2", f"-I{work}", f"-I{HOST / 'stub'}",
             f"-I{COMPONENT}", str(Path(__file__).with_name("plc_demo.cpp")),
             str(COMPONENT / "audio" / "audio_timeline.cpp"),
             str(COMPONENT / "audio" / "audio_plc.cpp"), "-o", str(binary)],
            check=True)
        subprocess.run([str(binary), str(wav), str(out_dir)], check=True)
    logger.info("wrote %s and %s", out_dir / "plc.wav", out_dir / "silence.wav")
    return 0


if __name__ == "__main__":
    sys.exit(main())
