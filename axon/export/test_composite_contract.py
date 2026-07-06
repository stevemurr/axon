"""Drift guard: _build_default_meta vs the SHIPPED weights/axon_bundle meta.

The controls dict in ``composite.py`` and the shipped
``weights/axon_bundle/axon_meta.json`` (which is what the plugin actually
loads, and what ``native/clap/tests/test_control_contract.cpp`` checks against
the C++ read-set) have drifted apart before — at worst composite.py emitted 9
dead controls and was missing 40 live ones, so any re-export following the
README flow would have shipped a bundle with MelLimiter stuck fully on and a
third of the knobs gone. This test pins the generator to the shipped artifact:

  * control key-sets must be identical, and
  * every control spec (name/min/max/default/skew/unit) must match.

Intentional control-set changes must update BOTH sides in lock-step (and the
C++ read-set + test_control_contract along with them).

Run directly (no pytest needed)::

    python3 axon/export/test_composite_contract.py

or via pytest from the repo root.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Allow running the file directly (sys.path[0] is axon/export/, not the repo
# root, so the ``axon`` package wouldn't otherwise resolve).
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from axon.export.composite import _build_default_meta  # noqa: E402
SHIPPED_META = REPO_ROOT / "weights" / "axon_bundle" / "axon_meta.json"

_NUMERIC_KEYS = ("min", "max", "default", "skew")
_STRING_KEYS = ("id", "name", "unit")


def _generated_controls(shipped: dict) -> dict:
    auto_eq = shipped.get("auto_eq", {})
    meta = _build_default_meta(
        model_id="test",
        sample_rate=int(shipped.get("sample_rate", 44100)),
        classes=auto_eq.get("class_order", ["full_mix"]),
        default_class=auto_eq.get("default_class", "full_mix"),
    )
    return meta.controls


def test_control_key_sets_match():
    shipped = json.loads(SHIPPED_META.read_text())
    gen = _generated_controls(shipped)
    shipped_ids = set(shipped["controls"].keys())
    gen_ids = set(gen.keys())
    missing = sorted(shipped_ids - gen_ids)   # would DROP these on re-export
    extra = sorted(gen_ids - shipped_ids)     # would ADD dead knobs
    assert not missing and not extra, (
        f"composite.py._build_default_meta drifted from {SHIPPED_META}:\n"
        f"  missing (re-export would drop): {missing}\n"
        f"  extra   (re-export would add dead knobs): {extra}"
    )


def test_control_specs_match():
    shipped = json.loads(SHIPPED_META.read_text())
    gen = _generated_controls(shipped)
    for cid, want in shipped["controls"].items():
        got = gen.get(cid)
        assert got is not None, f"{cid}: missing from _build_default_meta"
        for k in _STRING_KEYS:
            assert got[k] == want[k], (
                f"{cid}.{k}: generated {got[k]!r} != shipped {want[k]!r}")
        for k in _NUMERIC_KEYS:
            assert math.isclose(float(got[k]), float(want[k]),
                                rel_tol=0.0, abs_tol=0.0), (
                f"{cid}.{k}: generated {got[k]!r} != shipped {want[k]!r}")


def main() -> int:
    test_control_key_sets_match()
    test_control_specs_match()
    n = len(json.loads(SHIPPED_META.read_text())["controls"])
    print(f"[composite-contract] _build_default_meta == shipped axon_meta.json "
          f"({n} controls) PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
