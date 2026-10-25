"""airplay-measure: make the sweep file, then turn a phone recording of it into EQ.

    uv run airplay-measure sweep sweep.wav
    uv run airplay-measure analyze recording.m4a --high-pass 50 --out geneva/
"""

import argparse
import logging
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import orjson
from scipy.io import wavfile

from .biquad import GAINLESS, Filter, FilterKind, FloatArray
from .fit import Candidate, FitResult, FitSettings, fit
from .response import average_db, log_grid, normalize, position_responses, to_db
from .sweep import SweepPlan, deconvolve, find_impulses, program

logger = logging.getLogger("airplay_measure")

PCM16_FULL_SCALE = 32767
CHANNELS = ("both", "left", "right")
APO_TYPES = {FilterKind.PEAKING: "PK", FilterKind.LOW_SHELF: "LSC", FilterKind.HIGH_SHELF: "HSC",
             FilterKind.HIGH_PASS: "HPQ", FilterKind.LOW_PASS: "LPQ", FilterKind.NOTCH: "NO"}
PLOT_RANGE_DB = (-30.0, 15.0)


def _plan(args: argparse.Namespace) -> SweepPlan:
    return SweepPlan(repeats=args.repeats, sweep_seconds=args.sweep_seconds, gap_seconds=args.gap_seconds)


def write_sweep(path: Path, plan: SweepPlan, channel: str) -> None:
    """Write the test signal as 16-bit stereo WAV, on one or both channels."""
    mono = program(plan)
    silent = np.zeros_like(mono)
    left = mono if channel in ("both", "left") else silent
    right = mono if channel in ("both", "right") else silent
    pcm = np.round(np.stack((left, right), axis=1) * PCM16_FULL_SCALE).astype(np.int16)
    wavfile.write(path, plan.sample_rate, pcm)


def read_recording(path: Path, sample_rate: int) -> FloatArray:
    """Decode any format ffmpeg reads (Voice Memos .m4a included) to mono float.

    Raises:
        RuntimeError: if ffmpeg fails.
    """
    result = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(sample_rate),
                             "-f", "f32le", "-"], check=False, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg could not read {path}: {result.stderr.decode(errors='replace')}")
    return np.frombuffer(result.stdout, dtype="<f4").astype(np.float64)


def load_slots(path: Path, settings: FitSettings) -> list[Candidate]:
    """A fixed band layout, e.g. the HA sliders a board config exposes.

    The file is a JSON list of {"name", "type", "min_hz", "max_hz", "min_q",
    "max_q", "min_gain_db", "max_gain_db"}; gains are further capped by
    `settings`.

    Raises:
        ValueError: on an unknown filter type or a missing key.
    """
    slots = []
    for entry in orjson.loads(path.read_bytes()):
        try:
            gain = (max(entry["min_gain_db"], -settings.max_cut_db), min(entry["max_gain_db"], settings.max_boost_db))
            slots.append(Candidate(FilterKind(entry["type"]), (entry["min_q"], entry["max_q"]), gain,
                                   (entry["min_hz"], entry["max_hz"]), entry["name"]))
        except (KeyError, ValueError) as error:
            raise ValueError(f"{path}: bad slot {entry}: {error}") from error
    return slots


def _filter_dict(flt: Filter) -> dict[str, str | float]:
    entry: dict[str, str | float] = {"type": flt.kind.value, "frequency": f"{flt.frequency_hz:.0f}Hz",
                                     "q": round(flt.q, 2)}
    if flt.kind not in GAINLESS:
        entry["gain"] = f"{flt.gain_db:.1f}dB"
    return entry


def eq_yaml(filters: list[Filter], preamp_db: float, comment: str) -> str:
    """A `dsp:` block for the board config."""
    lines = [f"# {comment}", "dsp:", f"  preamp: {preamp_db:.1f}dB", "  filters:"]
    for flt in filters:
        entry = _filter_dict(flt)
        lines.append(f"    - type: {entry.pop('type')}")
        lines.extend(f"      {key}: {value}" for key, value in entry.items())
    return "\n".join(lines) + "\n"


def eq_apo(filters: list[Filter], preamp_db: float) -> str:
    """Equalizer APO / REW text export, the format the board import will read."""
    lines = [f"Preamp: {preamp_db:.1f} dB"]
    for number, flt in enumerate(filters, 1):
        gain = "" if flt.kind in GAINLESS else f" Gain {flt.gain_db:.1f} dB"
        lines.append(f"Filter {number}: ON {APO_TYPES[flt.kind]} Fc {flt.frequency_hz:.0f} Hz{gain} Q {flt.q:.2f}")
    return "\n".join(lines) + "\n"


def plot(path: Path, grid: FloatArray, positions: list[FloatArray], average: FloatArray, result: FitResult,
         title: str) -> None:
    """Measured positions, their average, the target and the predicted result."""
    fig, ax = plt.subplots(figsize=(11, 5.5))
    for index, position in enumerate(positions):
        ax.semilogx(grid, position, color="0.75", linewidth=0.8, label="positions" if index == 0 else None)
    ax.semilogx(grid, average, color="black", linewidth=1.6, label="measured (average)")
    ax.semilogx(grid, result.target_db, color="tab:green", linestyle="--", label="target")
    ax.semilogx(grid, result.predicted_db, color="tab:blue", linewidth=1.6, label="predicted with EQ")
    ax.axvspan(grid[0], result.low_hz, color="0.93")
    ax.axvspan(result.high_hz, grid[-1], color="0.93")
    ax.set(xlim=(grid[0], grid[-1]), ylim=PLOT_RANGE_DB, xlabel="Hz", ylabel="dB", title=title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def analyze(args: argparse.Namespace) -> int:
    plan = _plan(args)
    recording = read_recording(args.recording, plan.sample_rate)
    impulses = deconvolve(recording, plan)
    peaks = find_impulses(impulses, plan)
    if len(peaks) < plan.repeats:
        logger.warning("found %d of %d sweeps; the rest were too quiet or cut off", len(peaks), plan.repeats)
    grid = log_grid()
    powers = position_responses(impulses, peaks, plan.sample_rate, grid)
    average = average_db(grid, powers)
    positions = [normalize(grid, to_db(power)) for power in powers]

    fixed = [Filter(FilterKind.HIGH_PASS, args.high_pass)] if args.high_pass else []
    settings = FitSettings(low_hz=args.fit_low, high_hz=args.fit_high, max_bands=args.max_bands,
                           tilt_db_per_octave=args.tilt)
    slots = load_slots(args.slots, settings) if args.slots else None
    result = fit(grid, average, fixed, settings, plan.sample_rate, slots)

    args.out.mkdir(parents=True, exist_ok=True)
    filters = fixed + result.bands
    summary = (f"airplay-measure: {len(peaks)} positions, fit {result.low_hz:.0f}-{result.high_hz:.0f} Hz, "
               f"deviation {result.rms_before_db:.1f} -> {result.rms_after_db:.1f} dB RMS")
    (args.out / "eq.yaml").write_text(eq_yaml(filters, result.preamp_db, summary))
    (args.out / "ParametricEQ.txt").write_text(eq_apo(filters, result.preamp_db))
    plot(args.out / "response.png", grid, positions, average, result, summary)
    report = {"positions": len(peaks), "impulse_seconds": [p / plan.sample_rate for p in peaks],
              "fit_hz": [result.low_hz, result.high_hz], "rms_before_db": result.rms_before_db,
              "rms_after_db": result.rms_after_db, "preamp_db": result.preamp_db,
              "filters": [_filter_dict(f) for f in filters], "grid_hz": grid, "measured_db": average,
              "positions_db": positions, "target_db": result.target_db, "predicted_db": result.predicted_db}
    if slots is not None:
        fitted = dict(zip(result.slot_indices, result.bands))
        slot_values = {slot.name: ({"frequency_hz": round(fitted[i].frequency_hz),
                                    "q": round(fitted[i].q, 2), "gain_db": round(fitted[i].gain_db, 1)}
                                   if i in fitted else {"gain_db": 0.0})
                       for i, slot in enumerate(slots)}
        report["slots"] = slot_values
        for name, values in slot_values.items():
            logger.info("  slot %-10s %s", name, values)
    (args.out / "report.json").write_bytes(orjson.dumps(report, option=orjson.OPT_SERIALIZE_NUMPY))
    logger.info("%s", summary)
    for flt in filters:
        logger.info("  %s", eq_apo([flt], 0.0).splitlines()[1].split(": ", 1)[1])
    logger.info("wrote %s", args.out)
    return 0


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point."""
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    parser = argparse.ArgumentParser(prog="airplay-measure", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repeats", type=int, default=SweepPlan.repeats)
    parser.add_argument("--sweep-seconds", type=float, default=SweepPlan.sweep_seconds)
    parser.add_argument("--gap-seconds", type=float, default=SweepPlan.gap_seconds)
    commands = parser.add_subparsers(dest="command", required=True)

    make = commands.add_parser("sweep", help="write the test signal")
    make.add_argument("output", type=Path)
    make.add_argument("--channel", choices=CHANNELS, default="both")

    study = commands.add_parser("analyze", help="fit EQ to a recording of the test signal")
    study.add_argument("recording", type=Path)
    study.add_argument("--out", type=Path, default=Path("measurement"))
    study.add_argument("--high-pass", type=float, default=50.0,
                       help="protective high-pass kept in front of the fit, Hz (0: none)")
    study.add_argument("--fit-low", type=float, default=FitSettings.low_hz)
    study.add_argument("--fit-high", type=float, default=FitSettings.high_hz)
    study.add_argument("--max-bands", type=int, default=FitSettings.max_bands)
    study.add_argument("--tilt", type=float, default=FitSettings.tilt_db_per_octave,
                       help="target slope in dB per octave")
    study.add_argument("--slots", type=Path, help="JSON band layout to fill instead of free bands")
    args = parser.parse_args(argv)

    plan = _plan(args)
    if args.command == "sweep":
        write_sweep(args.output, plan, args.channel)
        total = plan.lead_seconds + plan.repeats * (plan.sweep_seconds + plan.gap_seconds)
        logger.info("wrote %s: %d sweeps, %.0f s", args.output, plan.repeats, total)
        return 0
    try:
        return analyze(args)
    except (RuntimeError, ValueError) as error:
        logger.error("%s", error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
