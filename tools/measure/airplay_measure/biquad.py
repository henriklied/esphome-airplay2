"""The board's biquads (RBJ cookbook, as design_biquad() in audio/audio_dsp.cpp builds them)."""

from dataclasses import dataclass
from enum import Enum

import numpy as np
import numpy.typing as npt

FloatArray = npt.NDArray[np.float64]

BUTTERWORTH_Q = 0.7071067811865476
# Floor for an exact null on the unit circle (notch, high-pass at DC).
SILENCE_DB = -200.0


class FilterKind(str, Enum):
    """Shapes the `dsp:` YAML accepts; the values are its `type:` strings."""

    LOW_SHELF = "low_shelf"
    HIGH_SHELF = "high_shelf"
    HIGH_PASS = "high_pass"
    LOW_PASS = "low_pass"
    PEAKING = "peaking"
    NOTCH = "notch"


GAINLESS = (FilterKind.HIGH_PASS, FilterKind.LOW_PASS, FilterKind.NOTCH)


@dataclass(frozen=True)
class Filter:
    """One biquad in human units, as a `dsp: filters:` entry."""

    kind: FilterKind
    frequency_hz: float
    q: float = BUTTERWORTH_Q
    gain_db: float = 0.0


def coefficients(flt: Filter, sample_rate: float) -> tuple[float, float, float, float, float]:
    """Normalized (b0, b1, b2, a1, a2) for `flt` at `sample_rate`."""
    w0 = 2.0 * np.pi * flt.frequency_hz / sample_rate
    cos_w0, sin_w0 = float(np.cos(w0)), float(np.sin(w0))
    alpha = sin_w0 / (2.0 * flt.q)
    amp = 10.0 ** (flt.gain_db / 40.0)
    if flt.kind is FilterKind.HIGH_PASS:
        k = (1.0 + cos_w0) * 0.5
        b0, b1, b2 = k, -(1.0 + cos_w0), k
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif flt.kind is FilterKind.LOW_PASS:
        k = (1.0 - cos_w0) * 0.5
        b0, b1, b2 = k, 1.0 - cos_w0, k
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif flt.kind is FilterKind.NOTCH:
        b0, b1, b2 = 1.0, -2.0 * cos_w0, 1.0
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif flt.kind is FilterKind.PEAKING:
        b0, b1, b2 = 1.0 + alpha * amp, -2.0 * cos_w0, 1.0 - alpha * amp
        a0, a1, a2 = 1.0 + alpha / amp, -2.0 * cos_w0, 1.0 - alpha / amp
    else:
        t = 2.0 * np.sqrt(amp) * alpha
        sign = 1.0 if flt.kind is FilterKind.LOW_SHELF else -1.0
        b0 = amp * ((amp + 1.0) - sign * (amp - 1.0) * cos_w0 + t)
        b1 = sign * 2.0 * amp * ((amp - 1.0) - sign * (amp + 1.0) * cos_w0)
        b2 = amp * ((amp + 1.0) - sign * (amp - 1.0) * cos_w0 - t)
        a0 = (amp + 1.0) + sign * (amp - 1.0) * cos_w0 + t
        a1 = -sign * 2.0 * ((amp - 1.0) + sign * (amp + 1.0) * cos_w0)
        a2 = (amp + 1.0) + sign * (amp - 1.0) * cos_w0 - t
    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


def response_db(filters: list[Filter], freqs_hz: FloatArray, sample_rate: float) -> FloatArray:
    """Magnitude of the whole cascade at `freqs_hz`, in dB."""
    total = np.zeros_like(freqs_hz)
    z = np.exp(-2j * np.pi * freqs_hz / sample_rate)
    for flt in filters:
        b0, b1, b2, a1, a2 = coefficients(flt, sample_rate)
        mag = np.abs((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z))
        total += np.where(mag > 0.0, 20.0 * np.log10(np.maximum(mag, 1e-300)), SILENCE_DB)
    return total
