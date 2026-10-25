"""End to end on a synthetic room: known filters in, the same response out, and
an EQ fit that flattens it."""

from pathlib import Path

import numpy as np
import pytest
from scipy.io import wavfile
from scipy.signal import lfilter

from airplay_measure.biquad import Filter, FilterKind, FloatArray, coefficients, response_db
from airplay_measure.cli import main, read_recording
from airplay_measure.fit import Candidate, FitSettings, fit
from airplay_measure.response import average_db, log_grid, normalize, position_responses
from airplay_measure.sweep import SweepPlan, deconvolve, find_impulses, program

PLAN = SweepPlan(repeats=3, sweep_seconds=3.0, gap_seconds=2.0)
CLI_TIMING = ["--repeats", "3", "--sweep-seconds", "3", "--gap-seconds", "2"]
LATENCY_SECONDS = 0.37  # AirPlay plus the phone, unknown to the analysis
NOISE_DBFS = -70.0
ROOM = [
    Filter(FilterKind.HIGH_PASS, 60.0, 0.7),        # the cabinet's own roll-off
    Filter(FilterKind.PEAKING, 120.0, 4.0, 8.0),    # a room mode
    Filter(FilterKind.PEAKING, 2000.0, 1.5, -5.0),  # a driver dip
]
CHECK_BAND_HZ = (100.0, 8000.0)


def _record(plan: SweepPlan, room: list[Filter], seed: int = 1) -> FloatArray:
    signal = program(plan)
    for flt in room:
        b0, b1, b2, a1, a2 = coefficients(flt, plan.sample_rate)
        signal = lfilter([b0, b1, b2], [1.0, a1, a2], signal)
    lead = np.zeros(round(LATENCY_SECONDS * plan.sample_rate))
    noise = np.random.default_rng(seed).normal(0.0, 10.0 ** (NOISE_DBFS / 20.0), lead.size + signal.size)
    return np.concatenate((lead, signal)) + noise


def _measure(recording: FloatArray) -> tuple[FloatArray, FloatArray, list[int]]:
    impulses = deconvolve(recording, PLAN)
    peaks = find_impulses(impulses, PLAN)
    grid = log_grid()
    return grid, average_db(grid, position_responses(impulses, peaks, PLAN.sample_rate, grid)), peaks


def test_finds_every_sweep() -> None:
    _, _, peaks = _measure(_record(PLAN, ROOM))
    assert len(peaks) == PLAN.repeats
    assert np.diff(peaks) == pytest.approx([PLAN.period_samples] * (PLAN.repeats - 1), abs=2)


def test_recovers_the_room() -> None:
    grid, measured, _ = _measure(_record(PLAN, ROOM))
    expected = normalize(grid, response_db(ROOM, grid, PLAN.sample_rate))
    band = (grid >= CHECK_BAND_HZ[0]) & (grid <= CHECK_BAND_HZ[1])
    assert np.max(np.abs(measured[band] - expected[band])) < 1.0


def test_fit_flattens_and_cuts_the_mode() -> None:
    grid, measured, _ = _measure(_record(PLAN, ROOM))
    result = fit(grid, measured, [Filter(FilterKind.HIGH_PASS, 50.0)], FitSettings(tilt_db_per_octave=0.0),
                 PLAN.sample_rate)
    assert result.rms_after_db < 1.0 < result.rms_before_db
    mode = [b for b in result.bands if 100.0 < b.frequency_hz < 145.0]
    assert mode and min(b.gain_db for b in mode) < -5.0
    assert all(b.gain_db <= FitSettings().max_boost_db + 1e-9 for b in result.bands)
    assert result.low_hz > 50.0  # above the cabinet's -6 dB point (~45 Hz): no boosting the roll-off


def test_preamp_keeps_cascade_below_full_scale() -> None:
    grid, measured, _ = _measure(_record(PLAN, ROOM))
    result = fit(grid, measured, [], FitSettings(), PLAN.sample_rate)
    peak = float(np.max(response_db(result.bands, grid, PLAN.sample_rate)))
    assert peak + result.preamp_db <= 0.0


def test_cli_round_trip(tmp_path: Path) -> None:
    sweep_path = tmp_path / "sweep.wav"
    assert main([*CLI_TIMING, "sweep", str(sweep_path)]) == 0
    rate, pcm = wavfile.read(sweep_path)
    assert rate == PLAN.sample_rate and pcm.shape[1] == 2
    played = pcm[:, 0].astype(np.float64) / 32767.0
    assert np.allclose(played, program(PLAN), atol=1.0 / 32767.0)

    recording = tmp_path / "recording.wav"
    wavfile.write(recording, PLAN.sample_rate, _record(PLAN, ROOM).astype(np.float32))
    assert read_recording(recording, PLAN.sample_rate).size > 0
    out = tmp_path / "out"
    assert main([*CLI_TIMING, "analyze", str(recording), "--out", str(out)]) == 0
    assert {p.name for p in out.iterdir()} == {"eq.yaml", "ParametricEQ.txt", "response.png", "report.json"}
    assert "type: high_pass" in (out / "eq.yaml").read_text()


def test_slots_stay_in_their_ranges() -> None:
    grid, measured, _ = _measure(_record(PLAN, ROOM))
    slots = [Candidate(FilterKind.LOW_SHELF, (0.7, 0.7), (-12.0, 4.0), (40.0, 500.0), "bass"),
             Candidate(FilterKind.PEAKING, (0.3, 8.0), (-12.0, 4.0), (200.0, 12000.0), "band")]
    result = fit(grid, measured, [], FitSettings(tilt_db_per_octave=0.0), PLAN.sample_rate, slots)
    assert sorted(result.slot_indices) == sorted(set(result.slot_indices))
    for index, band in zip(result.slot_indices, result.bands):
        lo, hi = slots[index].freq_range or (0.0, 0.0)
        assert band.kind is slots[index].kind and lo <= band.frequency_hz <= hi * 1.000001
    assert result.rms_after_db < result.rms_before_db


def test_matches_a_reference_speaker() -> None:
    grid, measured, _ = _measure(_record(PLAN, ROOM))
    reference_room = [*ROOM[:1], Filter(FilterKind.HIGH_SHELF, 3000.0, 0.7, -4.0)]
    _, reference, _ = _measure(_record(PLAN, reference_room, seed=2))
    result = fit(grid, measured, [], FitSettings(), PLAN.sample_rate, target_db=reference)
    assert result.rms_after_db < 1.0 < result.rms_before_db
    # The darker reference pulls the treble down rather than up.
    treble = (grid > 4000.0) & (grid < 8000.0)
    assert np.mean(result.predicted_db[treble] - measured[treble]) < np.mean(
        result.predicted_db[(grid > 300.0) & (grid < 1000.0)] - measured[(grid > 300.0) & (grid < 1000.0)])
