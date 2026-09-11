# /// script
# requires-python = ">=3.11"
# ///
"""Check the RBJ transcriptions in audio_dsp.cpp against their intended response.

The formulas are ported here expression-by-expression from design_biquad(); the
transfer function is then evaluated on the unit circle. A transcription typo
shows up as a response that misses its design points.
"""
import cmath
import math

FS = 44100.0


def design(kind: str, f0: float, q: float, gain_db: float) -> tuple[float, ...]:
    w0 = 2.0 * math.pi * f0 / FS
    cos_w0, sin_w0 = math.cos(w0), math.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    if kind == "high_pass":
        k = (1.0 + cos_w0) * 0.5
        b0, b1, b2 = k, -(1.0 + cos_w0), k
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif kind == "low_pass":
        k = (1.0 - cos_w0) * 0.5
        b0, b1, b2 = k, 1.0 - cos_w0, k
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif kind == "notch":
        b0, b1, b2 = 1.0, -2.0 * cos_w0, 1.0
        a0, a1, a2 = 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha
    elif kind == "peaking":
        amp = 10.0 ** (gain_db / 40.0)
        b0, b1, b2 = 1.0 + alpha * amp, -2.0 * cos_w0, 1.0 - alpha * amp
        a0, a1, a2 = 1.0 + alpha / amp, -2.0 * cos_w0, 1.0 - alpha / amp
    elif kind == "low_shelf":
        amp = 10.0 ** (gain_db / 40.0)
        t = 2.0 * math.sqrt(amp) * alpha
        b0 = amp * ((amp + 1.0) - (amp - 1.0) * cos_w0 + t)
        b1 = 2.0 * amp * ((amp - 1.0) - (amp + 1.0) * cos_w0)
        b2 = amp * ((amp + 1.0) - (amp - 1.0) * cos_w0 - t)
        a0 = (amp + 1.0) + (amp - 1.0) * cos_w0 + t
        a1 = -2.0 * ((amp - 1.0) + (amp + 1.0) * cos_w0)
        a2 = (amp + 1.0) + (amp - 1.0) * cos_w0 - t
    elif kind == "high_shelf":
        amp = 10.0 ** (gain_db / 40.0)
        t = 2.0 * math.sqrt(amp) * alpha
        b0 = amp * ((amp + 1.0) + (amp - 1.0) * cos_w0 + t)
        b1 = -2.0 * amp * ((amp - 1.0) + (amp + 1.0) * cos_w0)
        b2 = amp * ((amp + 1.0) + (amp - 1.0) * cos_w0 - t)
        a0 = (amp + 1.0) - (amp - 1.0) * cos_w0 + t
        a1 = 2.0 * ((amp - 1.0) - (amp + 1.0) * cos_w0)
        a2 = (amp + 1.0) - (amp - 1.0) * cos_w0 - t
    else:
        raise ValueError(kind)
    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


def response_db(coeffs: tuple[float, ...], freq: float) -> float:
    b0, b1, b2, a1, a2 = coeffs
    z = cmath.exp(-2j * math.pi * freq / FS)
    h = (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z)
    return 20.0 * math.log10(abs(h))


CASES = [
    # (label, kind, f0, q, gain, [(probe Hz, expected dB, tolerance)])
    ("X90 high-pass", "high_pass", 80.0, 0.7, 0.0,
     [(80.0, -3.0, 0.5), (20.0, -23.0, 3.0), (1000.0, 0.0, 0.1)]),
    ("Geneva high-pass", "high_pass", 45.0, 0.7, 0.0,
     [(45.0, -3.0, 0.5), (1000.0, 0.0, 0.1)]),
    ("X90 low shelf", "low_shelf", 150.0, 0.7, 4.0,
     [(10.0, 4.0, 0.15), (150.0, 2.0, 0.4), (8000.0, 0.0, 0.1)]),
    ("Geneva low shelf", "low_shelf", 100.0, 0.7, 4.0,
     [(10.0, 4.0, 0.15), (8000.0, 0.0, 0.1)]),
    ("high shelf +6", "high_shelf", 4000.0, 0.7, 6.0,
     [(20000.0, 6.0, 0.3), (50.0, 0.0, 0.1)]),
    ("peaking -6 @1k", "peaking", 1000.0, 2.0, -6.0,
     [(1000.0, -6.0, 0.05), (50.0, 0.0, 0.15), (15000.0, 0.0, 0.3)]),
    ("low-pass 5k", "low_pass", 5000.0, 0.7071, 0.0,
     [(5000.0, -3.0, 0.4), (100.0, 0.0, 0.1)]),
    # A notch puts a zero exactly on the unit circle, so the centre frequency
    # is -inf in exact arithmetic and float-precision-limited in practice. Only
    # the skirts carry a meaningful expectation.
    ("notch 1k", "notch", 1000.0, 5.0, 0.0,
     [(1000.0, -100.0, None), (100.0, 0.0, 0.2), (10000.0, 0.0, 0.2)]),
]

failures = 0
for label, kind, f0, q, gain, probes in CASES:
    coeffs = design(kind, f0, q, gain)
    for freq, expected, tol in probes:
        actual = response_db(coeffs, freq)
        # tol None means "at most expected" -- for a response with a true zero.
        ok = actual <= expected if tol is None else abs(actual - expected) <= tol
        failures += not ok
        want = f"<= {expected:+.0f}" if tol is None else f"{expected:+.1f} +-{tol}"
        print(f"{'ok  ' if ok else 'FAIL'} {label:20s} {freq:8.0f} Hz  {actual:+7.2f} dB  (want {want})")

# The shipped cascades, end to end, exactly as the device files declare them.
SHIPPED = {
    "X90": (-4.0, [
        ("high_pass", 80.0, 0.7, 0.0),
        ("low_shelf", 150.0, 0.7, 4.0),
        # Flat by default -- present only so both boards share a filter layout.
        ("high_shelf", 4000.0, 0.7, 0.0),
        ("peaking", 3800.0, 1.2, 0.0),
    ]),
    "Geneva": (-4.0, [
        ("high_pass", 45.0, 0.7, 0.0),
        ("low_shelf", 100.0, 0.7, 4.0),
        ("high_shelf", 3000.0, 0.7, -3.0),
        ("peaking", 3800.0, 1.2, -3.0),
    ]),
}

# A 0 dB shelf or peaking section must be an exact identity, or the "flat by
# default" sections on the X90 would colour it for nothing.
for kind in ("high_shelf", "low_shelf", "peaking"):
    coeffs = design(kind, 3000.0, 1.2, 0.0)
    worst = max(abs(response_db(coeffs, f)) for f in range(20, 20000, 10))
    ok = worst < 1e-9
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {kind + ' @0dB is identity':30s} worst |H| = {worst:.2e} dB")

for label, (preamp, sections) in SHIPPED.items():
    chain = [design(kind, f0, q, g) for kind, f0, q, g in sections]
    print(f"\n{label} cascade (preamp {preamp:+.0f} dB):")
    peak = max(preamp + sum(response_db(c, f) for c in chain) for f in range(20, 20000, 5))
    for f in (20, 40, 60, 80, 120, 150, 300, 1000, 2000, 3800, 6000, 12000):
        total = preamp + sum(response_db(c, f) for c in chain)
        print(f"  {f:6d} Hz  {total:+7.2f} dB")
    print(f"  peak across 20 Hz-20 kHz: {peak:+.2f} dB  ->  {'no clipping headroom lost' if peak <= 0.01 else 'CLIPS'}")
    failures += peak > 0.01

print("\nFAILURES:", failures)
raise SystemExit(1 if failures else 0)
