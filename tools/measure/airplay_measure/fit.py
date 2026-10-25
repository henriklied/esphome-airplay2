"""Fit peaking filters so measured response + board DSP follows a target curve.

Cuts are trusted more than boosts: a peak in a position-averaged in-room
measurement is real energy, while a dip is often a cancellation that a boost
only feeds. Boosts are therefore capped well below cuts, and nothing is
corrected outside [low_hz, high_hz] -- below the speaker's own roll-off, and
above where an uncalibrated phone microphone can be believed.
"""

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares

from .biquad import Filter, FilterKind, FloatArray, response_db
from .response import POINTS_PER_OCTAVE

PIVOT_HZ = 1000.0
OFFSET_LIMIT_DB = 40.0
# Speaker roll-off: the lowest frequency, scanning down from ROLLOFF_SCAN_HZ,
# before the response stays this far below the normalized midband.
ROLLOFF_DB = -3.0
ROLLOFF_SCAN_HZ = 300.0
ROLLOFF_SPAN_OCTAVES = 0.5
ROLLOFF_MARGIN = 1.1
PREAMP_STEP_DB = 0.5


@dataclass(frozen=True)
class FitSettings:
    """Limits on the fit. Defaults suit an uncalibrated phone microphone."""

    low_hz: float = 40.0
    high_hz: float = 10000.0
    max_bands: int = 7
    max_boost_db: float = 4.0
    max_cut_db: float = 12.0
    min_q: float = 0.4
    max_q: float = 5.0
    tilt_db_per_octave: float = -0.8
    stop_db: float = 1.0


@dataclass(frozen=True)
class FitResult:
    """Fitted bands and what they are predicted to do."""

    bands: list[Filter]
    preamp_db: float
    low_hz: float
    high_hz: float
    target_db: FloatArray
    predicted_db: FloatArray
    rms_before_db: float
    rms_after_db: float


def target_curve(grid: FloatArray, tilt_db_per_octave: float) -> FloatArray:
    """Straight tilt in log frequency through 0 dB at PIVOT_HZ."""
    return np.asarray(tilt_db_per_octave * np.log2(grid / PIVOT_HZ), dtype=np.float64)


def rolloff_hz(grid: FloatArray, measured_db: FloatArray) -> float:
    """Where the speaker stops playing: scanning down from ROLLOFF_SCAN_HZ, the
    first frequency below which the normalized response stays under ROLLOFF_DB
    for ROLLOFF_SPAN_OCTAVES. A room-mode dip is narrower than that."""
    span = round(ROLLOFF_SPAN_OCTAVES * POINTS_PER_OCTAVE)
    for index in np.flatnonzero(grid < ROLLOFF_SCAN_HZ)[::-1]:
        if index >= span and np.all(measured_db[index - span:index + 1] < ROLLOFF_DB):
            return float(grid[index])
    return float(grid[0])


def _bands(params: FloatArray) -> list[Filter]:
    return [Filter(FilterKind.PEAKING, float(np.exp(params[i])), float(params[i + 1]), float(params[i + 2]))
            for i in range(1, params.size, 3)]


def _rms(values: FloatArray) -> float:
    return float(np.sqrt(np.mean(values ** 2)))


def _q_for_width(octaves: float, settings: FitSettings) -> float:
    ratio = 2.0 ** max(octaves, 0.05)
    return float(np.clip(np.sqrt(ratio) / (ratio - 1.0), settings.min_q, settings.max_q))


def fit(grid: FloatArray, measured_db: FloatArray, fixed: list[Filter], settings: FitSettings,
        sample_rate: float) -> FitResult:
    """Fit up to `settings.max_bands` peaking filters on top of `fixed`.

    Args:
        grid: log-spaced frequencies of `measured_db`.
        measured_db: normalized, smoothed response measured with the DSP off.
        fixed: filters that stay regardless (the protective high-pass).
        settings: limits.
        sample_rate: the board's output rate.

    Returns:
        The bands, a preamp that keeps the cascade at or below 0 dB, and the
        predicted response.
    """
    low = max(settings.low_hz, rolloff_hz(grid, measured_db) * ROLLOFF_MARGIN)
    mask = (grid >= low) & (grid <= settings.high_hz)
    target = target_curve(grid, settings.tilt_db_per_octave)
    base = measured_db + response_db(fixed, grid, sample_rate)
    lower = [-OFFSET_LIMIT_DB]
    upper = [OFFSET_LIMIT_DB]

    def residual(params: FloatArray) -> FloatArray:
        eq = response_db(_bands(params), grid[mask], sample_rate)
        return np.asarray(base[mask] + eq + params[0] - target[mask], dtype=np.float64)

    params = np.array([float(np.mean(target[mask] - base[mask]))])
    rms_before = _rms(residual(params))
    for _ in range(settings.max_bands):
        error = residual(params)
        worst = int(np.argmax(np.abs(error)))
        if abs(error[worst]) < settings.stop_db:
            break
        # Width of the feature: where the error stays above half its peak.
        half = np.abs(error) >= abs(error[worst]) / 2.0
        lo = hi = worst
        while lo > 0 and half[lo - 1] and np.sign(error[lo - 1]) == np.sign(error[worst]):
            lo -= 1
        while hi < error.size - 1 and half[hi + 1] and np.sign(error[hi + 1]) == np.sign(error[worst]):
            hi += 1
        freqs = grid[mask]
        gain = float(np.clip(-error[worst], -settings.max_cut_db, settings.max_boost_db))
        q = _q_for_width(float(np.log2(freqs[hi] / freqs[lo])), settings)
        params = np.concatenate((params, [np.log(freqs[worst]), q, gain]))
        lower += [float(np.log(low)), settings.min_q, -settings.max_cut_db]
        upper += [float(np.log(settings.high_hz)), settings.max_q, settings.max_boost_db]
        params = least_squares(residual, np.clip(params, lower, upper), bounds=(lower, upper)).x

    bands = _bands(params)
    cascade = response_db(fixed + bands, grid, sample_rate)
    peak = float(np.max(cascade))
    preamp = -float(np.ceil(max(peak, 0.0) / PREAMP_STEP_DB) * PREAMP_STEP_DB)
    predicted = measured_db + cascade + params[0]
    return FitResult(bands=bands, preamp_db=preamp, low_hz=low, high_hz=settings.high_hz, target_db=target,
                     predicted_db=np.asarray(predicted, dtype=np.float64), rms_before_db=rms_before,
                     rms_after_db=_rms(residual(params)))
