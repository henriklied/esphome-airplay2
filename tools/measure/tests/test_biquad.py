"""The tool's biquads must be the board's: compare with tests/test_biquad_response.py,
which ports design_biquad() from audio/audio_dsp.cpp expression by expression."""

import importlib.util
from pathlib import Path

import numpy as np
import pytest

from airplay_measure.biquad import Filter, FilterKind, coefficients, response_db

FIRMWARE_PORT = Path(__file__).resolve().parents[3] / "tests" / "test_biquad_response.py"
SAMPLE_RATE = 44100.0


def _firmware_design():  # type: ignore[no-untyped-def]
    spec = importlib.util.spec_from_file_location("firmware_biquad", FIRMWARE_PORT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except SystemExit:
        pass  # it is a script: runs its checks and exits after defining design()
    assert module.FS == SAMPLE_RATE
    return module.design


@pytest.mark.parametrize("kind", list(FilterKind))
@pytest.mark.parametrize(("frequency", "q", "gain"), [(50.0, 0.7071, 0.0), (220.0, 1.3, 4.5), (8000.0, 0.9, -6.0)])
def test_matches_firmware_design(kind: FilterKind, frequency: float, q: float, gain: float) -> None:
    expected = _firmware_design()(kind.value, frequency, q, gain)
    assert coefficients(Filter(kind, frequency, q, gain), SAMPLE_RATE) == pytest.approx(expected, rel=1e-12)


def test_peaking_hits_its_gain() -> None:
    db = response_db([Filter(FilterKind.PEAKING, 1000.0, 2.0, -7.0)], np.array([1000.0, 20.0]), SAMPLE_RATE)
    assert db == pytest.approx([-7.0, 0.0], abs=0.05)
