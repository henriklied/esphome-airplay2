"""Impulse responses to smoothed, position-averaged magnitude responses."""

import numpy as np

from .biquad import FloatArray

PRE_PEAK_SECONDS = 0.005
# Long enough to hold the room's bass modes; fractional-octave smoothing then
# turns the reflections higher up into the steady-state in-room response.
WINDOW_SECONDS = 0.3
TAPER_FRACTION = 0.2
POINTS_PER_OCTAVE = 48
GRID_START_HZ = 20.0
GRID_STOP_HZ = 20000.0
# Smoothing widens from fine in the bass, where modes are narrow and real, to
# coarse in the treble, where narrow features are position-specific.
SMOOTHING_OCTAVES = ((200.0, 1.0 / 12.0), (2000.0, 1.0 / 3.0))
NORMALIZE_BAND_HZ = (300.0, 3000.0)
POWER_FLOOR = 1e-30


def log_grid(start_hz: float = GRID_START_HZ, stop_hz: float = GRID_STOP_HZ) -> FloatArray:
    """Log-spaced frequencies, POINTS_PER_OCTAVE per octave."""
    octaves = np.log2(stop_hz / start_hz)
    return np.asarray(start_hz * 2.0 ** (np.arange(int(octaves * POINTS_PER_OCTAVE) + 1) / POINTS_PER_OCTAVE),
                      dtype=np.float64)


def power_spectrum(impulses: FloatArray, peak: int, sample_rate: int) -> tuple[FloatArray, FloatArray]:
    """Windowed power spectrum of the impulse at `peak`.

    Returns:
        (frequencies in Hz, power) for the rfft bins.
    """
    pre = round(PRE_PEAK_SECONDS * sample_rate)
    length = round(WINDOW_SECONDS * sample_rate)
    start = max(peak - pre, 0)
    segment = impulses[start:start + length].copy()
    window = np.ones(segment.size)
    rise = min(pre, segment.size)
    window[:rise] = 0.5 - 0.5 * np.cos(np.pi * np.arange(rise) / max(rise, 1))
    taper = round(TAPER_FRACTION * segment.size)
    window[-taper:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(taper) / taper)
    size = 1 << int(np.ceil(np.log2(max(segment.size, 2) * 2)))
    spectrum = np.fft.rfft(segment * window, size)
    return np.fft.rfftfreq(size, 1.0 / sample_rate), np.abs(spectrum) ** 2


def smoothing_octaves(freq_hz: float) -> float:
    """Smoothing width at `freq_hz`, interpolated in log frequency."""
    (lo_hz, lo_width), (hi_hz, hi_width) = SMOOTHING_OCTAVES
    if freq_hz <= lo_hz:
        return lo_width
    if freq_hz >= hi_hz:
        return hi_width
    fraction = np.log(freq_hz / lo_hz) / np.log(hi_hz / lo_hz)
    return float(lo_width + fraction * (hi_width - lo_width))


def smooth(freqs: FloatArray, power: FloatArray, grid: FloatArray) -> FloatArray:
    """Fractional-octave power average of `power` at each `grid` frequency."""
    cumulative = np.concatenate(([0.0], np.cumsum(power)))
    out = np.empty_like(grid)
    for index, centre in enumerate(grid):
        half = smoothing_octaves(float(centre)) / 2.0
        lo = int(np.searchsorted(freqs, centre * 2.0 ** -half))
        hi = max(int(np.searchsorted(freqs, centre * 2.0 ** half)), lo + 1)
        out[index] = (cumulative[hi] - cumulative[lo]) / (hi - lo)
    return out


def to_db(power: FloatArray) -> FloatArray:
    return np.asarray(10.0 * np.log10(np.maximum(power, POWER_FLOOR)), dtype=np.float64)


def normalize(grid: FloatArray, db: FloatArray) -> FloatArray:
    """Shift `db` so its mean across NORMALIZE_BAND_HZ is 0 dB."""
    lo, hi = NORMALIZE_BAND_HZ
    band = (grid >= lo) & (grid <= hi)
    return np.asarray(db - float(np.mean(db[band])), dtype=np.float64)


def position_responses(impulses: FloatArray, peaks: list[int], sample_rate: int,
                       grid: FloatArray) -> list[FloatArray]:
    """Smoothed power response at each microphone position, on `grid`."""
    out = []
    for peak in peaks:
        freqs, power = power_spectrum(impulses, peak, sample_rate)
        out.append(smooth(freqs, power, grid))
    return out


def average_db(grid: FloatArray, powers: list[FloatArray]) -> FloatArray:
    """Power average across positions, normalized, in dB."""
    return normalize(grid, to_db(np.mean(np.stack(powers), axis=0)))
