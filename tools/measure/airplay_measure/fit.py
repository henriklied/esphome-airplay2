"""Fit peaking and shelving filters so measured response + board DSP follows a target.

Cuts are trusted more than boosts: a peak in a position-averaged in-room
measurement is real energy, while a dip is often a cancellation that a boost
only feeds. Boosts are therefore capped well below cuts, and nothing is
corrected outside [low_hz, high_hz] -- below the speaker's own roll-off, and
above where an uncalibrated phone microphone can be believed.
"""

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares

from .biquad import BUTTERWORTH_Q, Filter, FilterKind, FloatArray, response_db
from .response import POINTS_PER_OCTAVE

PIVOT_HZ = 1000.0
# Speaker roll-off: the lowest frequency, scanning down from ROLLOFF_SCAN_HZ,
# before the response stays this far below the normalized midband.
ROLLOFF_DB = -3.0
ROLLOFF_SCAN_HZ = 300.0
ROLLOFF_SPAN_OCTAVES = 0.5
ROLLOFF_MARGIN = 1.1
PREAMP_STEP_DB = 0.5
SHELF_Q_RANGE = (0.5, 1.0)
# A narrow boost lands in a cancellation, which no gain fills; only cuts may be narrow.
MAX_BOOST_Q = 1.5
BOOST_Q_PENALTY = 3.0
BOUND_EPSILON = 1e-6
SHELF_START_HZ = {FilterKind.LOW_SHELF: 300.0, FilterKind.HIGH_SHELF: 3000.0}
# Share of the fitted band allowed below the target before correction.
LEVEL_PERCENTILE = 25.0
# A band must cut the squared error by this share to be worth a biquad.
MIN_IMPROVEMENT = 0.05


@dataclass(frozen=True)
class FitSettings:
    """Limits on the fit. Defaults suit an uncalibrated phone microphone."""

    low_hz: float = 40.0
    high_hz: float = 8000.0
    max_bands: int = 7
    max_boost_db: float = 4.0
    max_cut_db: float = 12.0
    min_q: float = 0.4
    max_q: float = 5.0
    tilt_db_per_octave: float = -0.8
    stop_db: float = 1.0


@dataclass(frozen=True)
class Candidate:
    """A band shape the fit may add, with its own Q and gain range."""

    kind: FilterKind
    q_range: tuple[float, float]
    gain_range: tuple[float, float]
    freq_range: tuple[float, float] | None = None
    name: str = ""


def candidates(settings: FitSettings) -> list[Candidate]:
    """Narrow or broad cuts, broad boosts, and shelves either way."""
    either = (-settings.max_cut_db, settings.max_boost_db)
    return [
        Candidate(FilterKind.PEAKING, (settings.min_q, settings.max_q), (-settings.max_cut_db, 0.0)),
        Candidate(FilterKind.PEAKING, (settings.min_q, MAX_BOOST_Q), (0.0, settings.max_boost_db)),
        Candidate(FilterKind.LOW_SHELF, SHELF_Q_RANGE, either),
        Candidate(FilterKind.HIGH_SHELF, SHELF_Q_RANGE, either),
    ]


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
    # With fixed slots: the slot index of each band in `bands`.
    slot_indices: list[int]


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


def _filters(bands: list[Candidate], params: FloatArray) -> list[Filter]:
    return [Filter(band.kind, float(np.exp(params[3 * i])), float(params[3 * i + 1]), float(params[3 * i + 2]))
            for i, band in enumerate(bands)]


def _bounds(bands: list[Candidate], low_hz: float, high_hz: float) -> tuple[list[float], list[float]]:
    lower: list[float] = []
    upper: list[float] = []
    for band in bands:
        f_lo, f_hi = band.freq_range or (low_hz, high_hz)
        # least_squares needs lower < upper; a slot may pin Q (a shelf slider has none).
        lows = [float(np.log(f_lo)), band.q_range[0], band.gain_range[0]]
        highs = [float(np.log(f_hi)), band.q_range[1], band.gain_range[1]]
        lower += lows
        upper += [hi if hi > lo else lo + BOUND_EPSILON for lo, hi in zip(lows, highs)]
    return lower, upper


def _boost_penalty(bands: list[Candidate], params: FloatArray) -> FloatArray:
    """Residual terms that make a narrow peaking boost expensive (see MAX_BOOST_Q)."""
    terms = [BOOST_Q_PENALTY * max(params[3 * i + 2], 0.0) * max(params[3 * i + 1] - MAX_BOOST_Q, 0.0)
             for i, band in enumerate(bands) if band.kind is FilterKind.PEAKING]
    return np.asarray(terms, dtype=np.float64)


def _deviation(error: FloatArray) -> float:
    """RMS of the error once its mean is removed: shape, not level."""
    return float(np.sqrt(np.mean((error - np.mean(error)) ** 2)))


def _q_for_width(octaves: float, settings: FitSettings) -> float:
    ratio = 2.0 ** max(octaves, 0.05)
    return float(np.clip(np.sqrt(ratio) / (ratio - 1.0), settings.min_q, settings.max_q))


def _initial(band: Candidate, error: FloatArray, freqs: FloatArray, settings: FitSettings) -> list[float]:
    """Starting point for one more `band` against the current error."""
    kind = band.kind

    def clamp(gain: float) -> float:
        return float(np.clip(gain, *band.gain_range))

    if kind is FilterKind.LOW_SHELF:
        side = freqs < SHELF_START_HZ[kind]
        return [float(np.log(SHELF_START_HZ[kind])), BUTTERWORTH_Q, clamp(-float(np.mean(error[side])))]
    if kind is FilterKind.HIGH_SHELF:
        side = freqs > SHELF_START_HZ[kind]
        return [float(np.log(SHELF_START_HZ[kind])), BUTTERWORTH_Q, clamp(-float(np.mean(error[side])))]
    # The worst error this band can act on: peaks for a cut, dips for a boost.
    reachable = error if band.gain_range[1] <= 0.0 else -error
    worst = int(np.argmax(reachable))
    # Width of the feature: where the error stays above half its peak, same sign.
    same = (np.abs(error) >= abs(error[worst]) / 2.0) & (np.sign(error) == np.sign(error[worst]))
    lo = hi = worst
    while lo > 0 and same[lo - 1]:
        lo -= 1
    while hi < error.size - 1 and same[hi + 1]:
        hi += 1
    q = _q_for_width(float(np.log2(freqs[hi] / freqs[lo])), settings)
    return [float(np.log(freqs[worst])), q, clamp(-float(error[worst]))]


def fit(grid: FloatArray, measured_db: FloatArray, fixed: list[Filter], settings: FitSettings,
        sample_rate: float, slots: list[Candidate] | None = None,
        target_db: FloatArray | None = None) -> FitResult:
    """Fit up to `settings.max_bands` peaking and shelving filters on top of `fixed`.

    Each step tries one more band of every shape in candidates(), optimizes all
    bands jointly, and keeps the shape that lowers the error most. The target's
    level is fixed so LEVEL_PERCENTILE of the band lies below the response:
    the fit then corrects with cuts, and boosts only fill what is left.

    Args:
        grid: log-spaced frequencies of `measured_db`.
        measured_db: normalized, smoothed response measured with the DSP off.
        fixed: filters that stay regardless (the protective high-pass).
        settings: limits.
        sample_rate: the board's output rate.
        slots: a fixed layout to fill instead (the HA sliders a board exposes);
            each slot is used at most once, within its own ranges.
        target_db: a reference response to match (another speaker measured
            the same way) instead of the tilt; only its shape matters.

    Returns:
        The bands, a preamp that keeps the cascade at or below 0 dB, and the
        predicted response.
    """
    low = max(settings.low_hz, rolloff_hz(grid, measured_db) * ROLLOFF_MARGIN)
    mask = (grid >= low) & (grid <= settings.high_hz)
    freqs = grid[mask]
    target = target_curve(grid, settings.tilt_db_per_octave) if target_db is None else target_db
    base = measured_db + response_db(fixed, grid, sample_rate)
    offset = -float(np.percentile(base[mask] - target[mask], LEVEL_PERCENTILE))

    def residual(bands: list[Candidate], params: FloatArray) -> FloatArray:
        eq = response_db(_filters(bands, params), freqs, sample_rate)
        error = base[mask] + offset + eq - target[mask]
        return np.asarray(np.concatenate((error, _boost_penalty(bands, params))), dtype=np.float64)

    def error_only(bands: list[Candidate], params: FloatArray) -> FloatArray:
        return residual(bands, params)[:freqs.size]

    kinds: list[Candidate] = []
    params = np.empty(0)
    used: list[int] = []
    cost = float(np.sum(residual(kinds, params) ** 2))
    rms_before = _deviation(error_only(kinds, params))
    pool = slots if slots is not None else candidates(settings)
    for _ in range(min(settings.max_bands, len(pool)) if slots is not None else settings.max_bands):
        error = error_only(kinds, params)
        if float(np.max(np.abs(error))) < settings.stop_db:
            break
        best: tuple[float, list[Candidate], FloatArray, int] | None = None
        for index, kind in enumerate(pool):
            if slots is not None and index in used:
                continue
            trial_kinds = kinds + [kind]
            lower, upper = _bounds(trial_kinds, low, settings.high_hz)
            start = np.clip(np.concatenate((params, _initial(kind, error, freqs, settings))), lower, upper)
            solved = least_squares(lambda p, k=trial_kinds: residual(k, p), start, bounds=(lower, upper))
            trial_cost = float(np.sum(solved.fun ** 2))
            if best is None or trial_cost < best[0]:
                best = (trial_cost, trial_kinds, solved.x, index)
        if best is None or best[0] > cost * (1.0 - MIN_IMPROVEMENT):
            break
        cost, kinds, params, chosen = best
        used.append(chosen)

    bands = _filters(kinds, params)
    cascade = response_db(fixed + bands, grid, sample_rate)
    preamp = -float(np.ceil(max(float(np.max(cascade)), 0.0) / PREAMP_STEP_DB) * PREAMP_STEP_DB)
    return FitResult(bands=bands, preamp_db=preamp, low_hz=low, high_hz=settings.high_hz, target_db=target,
                     predicted_db=np.asarray(measured_db + cascade + offset, dtype=np.float64),
                     rms_before_db=rms_before, rms_after_db=_deviation(error_only(kinds, params)),
                     slot_indices=used if slots is not None else [])
