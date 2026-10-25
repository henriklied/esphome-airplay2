"""Exponential sine sweeps (Farina) and their deconvolution into impulse responses.

The test signal is several identical sweeps separated by silence, so one
recording covers several microphone positions. Deconvolving the whole
recording with one sweep's inverse filter yields one impulse per sweep; the
harmonic distortion products land well before each, outside its window.
"""

from dataclasses import dataclass

import numpy as np
from scipy.signal import fftconvolve

from .biquad import FloatArray

FADE_SECONDS = 0.05
# A peak this far below the strongest one is not a sweep (noise, a door).
IMPULSE_FLOOR_DB = -20.0
# Clock drift between the sender and the phone moves later sweeps slightly.
IMPULSE_SEARCH_SECONDS = 0.1


@dataclass(frozen=True)
class SweepPlan:
    """Shape of the test signal. Analysis must use the plan the file was made with."""

    sample_rate: int = 44100
    start_hz: float = 30.0
    end_hz: float = 20000.0
    sweep_seconds: float = 6.0
    gap_seconds: float = 6.0
    lead_seconds: float = 3.0
    repeats: int = 5
    level_dbfs: float = -15.0

    @property
    def period_samples(self) -> int:
        return round((self.sweep_seconds + self.gap_seconds) * self.sample_rate)

    @property
    def rate_constant(self) -> float:
        """L in x(t) = sin(2 pi f1 L (e^(t/L) - 1))."""
        return self.sweep_seconds / float(np.log(self.end_hz / self.start_hz))


def sweep(plan: SweepPlan) -> FloatArray:
    """One faded exponential sweep at `plan.level_dbfs`."""
    t = np.arange(round(plan.sweep_seconds * plan.sample_rate)) / plan.sample_rate
    length = plan.rate_constant
    signal = np.sin(2.0 * np.pi * plan.start_hz * length * (np.exp(t / length) - 1.0))
    fade = round(FADE_SECONDS * plan.sample_rate)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    signal[:fade] *= ramp
    signal[-fade:] *= ramp[::-1]
    return np.asarray(signal * 10.0 ** (plan.level_dbfs / 20.0), dtype=np.float64)


def program(plan: SweepPlan) -> FloatArray:
    """The whole test signal: lead silence, then `repeats` x (sweep, gap)."""
    one = sweep(plan)
    out = np.zeros(round(plan.lead_seconds * plan.sample_rate) + plan.repeats * plan.period_samples)
    start = round(plan.lead_seconds * plan.sample_rate)
    for index in range(plan.repeats):
        offset = start + index * plan.period_samples
        out[offset:offset + one.size] = one
    return out


def inverse_filter(plan: SweepPlan) -> FloatArray:
    """Time-reversed sweep with the -6 dB/octave envelope that whitens it.

    Scaled so a sweep deconvolved with it has unit gain at 1 kHz.
    """
    one = sweep(plan)
    t = np.arange(one.size) / plan.sample_rate
    # The reversed sweep starts at the top; attenuating as it descends gives
    # amplitude proportional to frequency, which cancels the sweep's pink tilt.
    inverse = one[::-1] * np.exp(-t / plan.rate_constant)
    impulse = fftconvolve(one, inverse)
    spectrum = np.abs(np.fft.rfft(impulse))
    freqs = np.fft.rfftfreq(impulse.size, 1.0 / plan.sample_rate)
    return np.asarray(inverse / spectrum[np.argmin(np.abs(freqs - 1000.0))], dtype=np.float64)


def deconvolve(recording: FloatArray, plan: SweepPlan) -> FloatArray:
    """Impulse train: one impulse response per sweep in `recording`."""
    return np.asarray(fftconvolve(recording, inverse_filter(plan)), dtype=np.float64)


def find_impulses(impulses: FloatArray, plan: SweepPlan) -> list[int]:
    """Indices of each sweep's main impulse, in time order.

    Raises:
        ValueError: if no impulse stands out of the noise.
    """
    envelope = np.abs(impulses)
    strongest = int(np.argmax(envelope))
    floor = envelope[strongest] * 10.0 ** (IMPULSE_FLOOR_DB / 20.0)
    search = round(IMPULSE_SEARCH_SECONDS * plan.sample_rate)
    found: list[int] = []
    for step in range(-(plan.repeats - 1), plan.repeats):
        centre = strongest + step * plan.period_samples
        lo, hi = max(centre - search, 0), min(centre + search, envelope.size)
        if lo >= hi:
            continue
        peak = lo + int(np.argmax(envelope[lo:hi]))
        if envelope[peak] >= floor:
            found.append(peak)
    if not found:
        raise ValueError("no sweep found in the recording")
    return sorted(set(found))
