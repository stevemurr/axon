"""Composite Axon plugin export.

Reads pre-built per-stage bundles (saturator and N per-class auto_eq —
all produced by ``nablafx-export``) and emits a single staging directory ready
for ``native/clap/build.sh axon`` on macOS.

Composite layout written to ``out_dir``::

    axon_meta.json                # this module's schema; see CompositePluginMeta
    saturator/                    (no model.onnx — pure DSP stage)
    ssl_comp/                     (copied from input bundle)
    auto_eq_<class>/              (N copies, one per class — controller LSTM ONNX
                                  + identical PEQ DSP block in plugin_meta.json)

The C++ side (``native/clap/src/composite_meta.cpp``) loads ``axon_meta.json``,
the saturator sub-``plugin_meta.json``, and one auto_eq sub-meta per
class. All auto_eq classes must share the same PEQ DSP layout (frozen freqs
+ identical ranges) so the runtime can swap which controller ONNX is active
without changing the downstream biquad cascade.

Schema versions:
    1 — single-class auto_eq (deprecated; brown-noise direction)
    2 — multi-class auto_eq with per-class controller bundles + CLS control
"""

from __future__ import annotations

import json
import shutil
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Tuple


SCHEMA_VERSION = 2


@dataclass(frozen=True)
class _AmountMappingSat:
    pre_gain_db_max:  float = 12.0
    post_gain_db_max: float = -12.0
    wet_mix_max:      float = 1.0


@dataclass(frozen=True)
class _AmountMappingAutoEq:
    wet_mix_max: float = 1.0


@dataclass(frozen=True)
class _AmountMappingSslComp:
    wet_mix_max: float = 1.0   # SSC knob → ssl_comp wet/dry mix


@dataclass(frozen=True)
class CompositePluginMeta:
    """Top-level meta for the composite Axon plugin.

    The C++ host reads this once at module load to wire AMT → per-stage params,
    locate sub-bundles, and configure the in-host DSP stages (LUFS leveler,
    true-peak ceiling, output trim).
    """
    schema_version: int = SCHEMA_VERSION
    effect_name:    str = "Axon"
    model_id:       str = ""
    sample_rate:    int = 44100
    channels:       int = 1
    # Single-instance sub-bundles (saturator, ssl_comp). Directory names relative
    # to the staging dir / the .clap Resources dir.
    sub_bundles:    Dict[str, str] = field(default_factory=dict)
    # Multi-class auto-EQ — one bundle per instrument-class preset.
    auto_eq:        Dict[str, Any] = field(default_factory=dict)
    # AMT (Amount), TRM (Output Trim), CLS (auto-EQ class) etc.
    controls:       Dict[str, Dict[str, Any]] = field(default_factory=dict)
    # Per-stage mapping from AMT ∈ [0, 1] to internal parameters.
    amount_mapping: Dict[str, Dict[str, float]] = field(default_factory=dict)
    leveler:        Dict[str, float] = field(default_factory=dict)
    ceiling:        Dict[str, float] = field(default_factory=dict)


# Stable canonical class order; bundle layout uses this for `min`/`max` of
# the CLS control and matches the C++ side's index-to-class lookup.
DEFAULT_CLASS_ORDER: Tuple[str, ...] = ("bass", "drums", "vocals", "other", "full_mix")
DEFAULT_ACTIVE_CLASS: str = "full_mix"


def _ctl(cid: str, name: str, mn: float, mx: float, default: float,
         unit: str = "", skew: float = 1.0) -> Dict[str, Any]:
    return {"id": cid, "name": name, "min": mn, "max": mx,
            "default": default, "skew": skew, "unit": unit}


def _build_default_meta(
    model_id: str,
    sample_rate: int,
    classes: Iterable[str],
    default_class: str,
) -> CompositePluginMeta:
    classes = list(classes)
    if default_class not in classes:
        raise ValueError(f"default_class {default_class!r} not in classes {classes!r}")
    n_classes = len(classes)
    default_idx = classes.index(default_class)
    auto_eq = {
        "default_class": default_class,
        "classes":       {c: f"auto_eq_{c}" for c in classes},
        # Stable ordering — the CLS control's integer value indexes into this.
        "class_order":   list(classes),
    }
    # CONTRACT: this list must equal, exactly, the set of controls the C++
    # plugin reads (the `c.id == "..."` compares in
    # native/clap/src/axon_plugin.cpp — resolve_amount_ et al.). Two guards
    # pin it:
    #   - native/clap/tests/test_control_contract.cpp diffs the SHIPPED
    #     weights/axon_bundle/axon_meta.json against the C++ read-set (and
    #     native/clap/build.sh runs it on every build);
    #   - axon/export/test_composite_contract.py diffs THIS function's output
    #     against the shipped json, so a re-export can never silently drop or
    #     resurrect controls (this drifted badly once: 9 dead controls
    #     emitted, 40 live ones missing).
    # export_composite_bundle() additionally refuses to overwrite an existing
    # axon_meta.json with a different control set unless explicitly allowed.
    control_list: List[Dict[str, Any]] = [
        # Saturator (RationalA + drive/trim/mix/filters).
        _ctl("SDR", "Sat Drive",   0.0,   24.0,  0.0, "dB"),
        _ctl("SVO", "Sat Output", -24.0,  12.0,  0.0, "dB"),
        _ctl("SMX", "Sat Mix",     0.0,    1.0,  0.0),
        _ctl("SHF", "Sat HPF",    20.0,  500.0, 20.0, "Hz"),
        _ctl("SLF", "Sat LPF",  1000.0, 20000.0, 20000.0, "Hz"),
        _ctl("STH", "Sat Thresh", -24.0,   0.0,  0.0, "dB"),
        _ctl("SBS", "Sat Bias",   -0.5,    0.5,  0.0),
        # SSL bus comp: wet mix + input trim feeding the fixed-curve model
        # (sets its operating point; the reciprocal make-up is applied to the
        # wet output in the plugin so a static trim is level-neutral).
        _ctl("SSC",    "Bus Comp",  0.0,  1.0, 1.0, "switch"),
        _ctl("SSC_IN", "Input",   -24.0, 12.0, 0.0, "dB"),
        # Auto-EQ: class select, wet mix, range/boost/speed shaping, engine
        # (neural LSTM vs deterministic cascade), renderer (STFT mask vs
        # zero-latency IIR bank), freeze (hold last live-solved curve).
        _ctl("CLS", "EQ Class", 0.0, float(max(0, n_classes - 1)),
             float(default_idx), "enum"),
        _ctl("EQ",        "Auto EQ",        0.0,   1.0,   1.0),
        _ctl("EQR",       "EQ Range",       0.0,   1.0,   1.0),
        _ctl("EQB",       "EQ Boost",       0.0,   1.0,   1.0),
        _ctl("EQS",       "EQ Speed",      10.0, 500.0, 100.0, "ms"),
        _ctl("EQ_ENGINE", "AutoEQ Engine",  0.0,   1.0,   0.0, "switch"),
        _ctl("EQ_RENDER", "AutoEQ Renderer",0.0,   1.0,   1.0, "switch"),
        _ctl("EQ_FREEZE", "AutoEQ Freeze",  0.0,   1.0,   0.0, "switch"),
        # Output trim.
        _ctl("TRM", "Output Trim", -12.0, 12.0, 0.0, "dB"),
        # Mel limiter.
        _ctl("MLI", "Limiter",         0.0,  1.0, 1.0),
        _ctl("MLC", "Ceiling",       -12.0,  0.0, 0.0, "dBFS"),
        _ctl("MLD", "Drive",           0.0, 24.0, 2.0, "dB"),
        _ctl("MLG", "Adaptive Gain",   0.0,  1.0, 0.5),
        _ctl("MLS", "Adaptive Speed",  0.0,  1.0, 0.5),
        _ctl("MLA", "Dynamic",         0.0,  1.0, 1.0, "switch"),
        # Bass mono (mono-below-cutoff).
        _ctl("BMI", "Bass Mono",  0.0,   1.0,   1.0, "switch"),
        _ctl("BMF", "Frequency", 20.0, 500.0, 225.0, "Hz"),
        # Transparent mastering room reverb (8-line FDN).
        # RVB_MIX is the parallel wet blend (0 = bypass, bit-identical).
        _ctl("RVB_MIX",    "Mix",       0.0,     1.0,    1.0),
        _ctl("RVB_SIZE",   "Size",      0.0,     1.0,    0.30),
        _ctl("RVB_WIDTH",  "Width",     0.0,     1.0,    1.0),
        _ctl("RVB_DAMP",   "Damp",   2000.0, 18000.0, 7000.0, "Hz"),
        _ctl("RVB_LOWCUT", "Low Cut",  20.0,  1000.0,  250.0, "Hz"),
        # Transparent M/S stereo widener (frequency-dependent side gain;
        # mono-safe). WID_ON toggles the stage; WID_AMT is the side width
        # gain (1 = neutral).
        _ctl("WID_ON",   "Width",   0.0,    1.0,   1.0, "switch"),
        _ctl("WID_AMT",  "Amount",  0.0,    2.0,   1.38),
        _ctl("WID_FREQ", "Low",    50.0, 1000.0, 250.0, "Hz"),
        _ctl("WID_AIR",  "Air",     0.0,    1.0,   1.0),
        # Auto gain (level match) + master bypass.
        _ctl("AGN", "Auto Gain", 0.0, 1.0, 1.0, "switch"),
        _ctl("BYP", "Bypass",    0.0, 1.0, 0.0, "switch"),
        # SSL 9000 J channel EQ (SEQ_*). SEQ_ON defaults OFF so the stage is
        # a bit-identical bypass out of the box. SEQ_AUTO/SPLIT/CAL/RESET are
        # the Auto-EQ coupling (assist bands absorb the Auto-EQ correction).
        _ctl("SEQ_ON",      "EQ",            0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_LF_G",    "LF Gain",     -18.0,    18.0,     0.0, "dB"),
        _ctl("SEQ_LF_F",    "LF Freq",      30.0,   600.0,   100.0, "Hz"),
        _ctl("SEQ_LF_BELL", "LF Bell",       0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_LMF_G",   "LMF Gain",    -18.0,    18.0,     0.0, "dB"),
        _ctl("SEQ_LMF_F",   "LMF Freq",     60.0,  3000.0,   500.0, "Hz"),
        _ctl("SEQ_LMF_Q",   "LMF Q",         0.1,     4.0,     1.0),
        _ctl("SEQ_HMF_G",   "HMF Gain",    -18.0,    18.0,     0.0, "dB"),
        _ctl("SEQ_HMF_F",   "HMF Freq",    400.0, 20000.0,  3000.0, "Hz"),
        _ctl("SEQ_HMF_Q",   "HMF Q",         0.1,     4.0,     1.0),
        _ctl("SEQ_HF_G",    "HF Gain",     -18.0,    18.0,     0.0, "dB"),
        _ctl("SEQ_HF_F",    "HF Freq",    1500.0, 20000.0, 10000.0, "Hz"),
        _ctl("SEQ_HF_BELL", "HF Bell",       0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_HPF_ON",  "HPF",           0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_HPF_F",   "HPF Freq",     20.0,   500.0,    80.0, "Hz"),
        _ctl("SEQ_LPF_ON",  "LPF",           0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_LPF_F",   "LPF Freq",   3000.0, 22000.0, 20000.0, "Hz"),
        _ctl("SEQ_DRIVE",   "Colour",        0.0,     1.0,     0.0),
        _ctl("SEQ_AUTO",    "Auto Assist",   0.0,     1.0,     0.0),
        _ctl("SEQ_SPLIT",   "Split",         0.0,     1.0,     0.6),
        _ctl("SEQ_CAL",     "Recalibrate",   0.0,     1.0,     0.0, "switch"),
        _ctl("SEQ_RESET",   "Reset",         0.0,     1.0,     0.0, "switch"),
    ]
    controls = {c["id"]: c for c in control_list}
    return CompositePluginMeta(
        model_id=model_id,
        sample_rate=sample_rate,
        sub_bundles={
            "saturator": "saturator",
            "ssl_comp":  "ssl_comp",
        },
        auto_eq=auto_eq,
        controls=controls,
        amount_mapping={
            "saturator": asdict(_AmountMappingSat()),
            "auto_eq":   asdict(_AmountMappingAutoEq()),
            "ssl_comp":  asdict(_AmountMappingSslComp()),
        },
        leveler={"target_lufs": -14.0},
        ceiling={"ceiling_dbtp": -1.0, "lookahead_ms": 1.5,
                 "attack_ms": 0.5, "release_ms": 50.0},
    )


def _load_sub_meta(bundle_dir: Path) -> Dict[str, Any]:
    p = bundle_dir / "plugin_meta.json"
    if not p.is_file():
        raise FileNotFoundError(f"missing {p}")
    return json.loads(p.read_text())


def _check_sub_bundle(bundle_dir: Path, expected_kind: str, expected_block_kind: Optional[str] = None) -> Dict[str, Any]:
    meta = _load_sub_meta(bundle_dir)
    sk = meta.get("stage_kind")
    if sk != expected_kind:
        raise ValueError(f"{bundle_dir}: expected stage_kind={expected_kind!r}, got {sk!r}")
    if expected_block_kind:
        blocks = meta.get("dsp_blocks") or []
        if not blocks or blocks[0].get("kind") != expected_block_kind:
            raise ValueError(
                f"{bundle_dir}: expected dsp_blocks[0].kind={expected_block_kind!r}, "
                f"got {[b.get('kind') for b in blocks]}"
            )
    return meta


def export_composite_bundle(
    auto_eq_bundles: Mapping[str, Path],
    saturator_bundle: Path,
    ssl_comp_bundle: Path,
    out_dir: Path,
    effect_name: str = "Axon",
    default_class: str = DEFAULT_ACTIVE_CLASS,
    class_order: Optional[List[str]] = None,
    allow_control_set_change: bool = False,
) -> CompositePluginMeta:
    """Validate sub-bundles, copy them under ``out_dir``, and write
    ``axon_meta.json``.

    ``auto_eq_bundles`` is a mapping of class name → directory holding a
    ``nablafx-export`` bundle for that class's controller+DSP. All classes must
    share the same SpectralMaskEQ geometry.

    If ``out_dir`` already holds an ``axon_meta.json`` (the normal case: the
    canonical flow re-exports over ``weights/axon_bundle``), the new control
    set must equal the existing one — this module has silently drifted from
    the C++ read-set before, and a drifted re-export builds and installs
    cleanly while stages stall at their defaults. Pass
    ``allow_control_set_change=True`` for an intentional control-set change
    (and update the C++ + run native/clap tests in lock-step).
    """
    if not auto_eq_bundles:
        raise ValueError("auto_eq_bundles must contain at least one class")

    # Resolve class order: caller-provided > canonical (filtered to bundles given)
    # > insertion order from the dict.
    if class_order is None:
        ordered = [c for c in DEFAULT_CLASS_ORDER if c in auto_eq_bundles]
        # Append any extra classes not in the canonical list, preserving caller order.
        for c in auto_eq_bundles.keys():
            if c not in ordered:
                ordered.append(c)
    else:
        for c in class_order:
            if c not in auto_eq_bundles:
                raise ValueError(f"class_order entry {c!r} not in auto_eq_bundles")
        for c in auto_eq_bundles.keys():
            if c not in class_order:
                raise ValueError(f"auto_eq_bundles key {c!r} missing from class_order")
        ordered = list(class_order)

    if default_class not in ordered:
        raise ValueError(f"default_class {default_class!r} not in classes {ordered!r}")

    auto_eq_paths = {c: Path(auto_eq_bundles[c]).resolve() for c in ordered}
    saturator_bundle  = Path(saturator_bundle).resolve()
    ssl_comp_bundle   = Path(ssl_comp_bundle).resolve()
    out_dir = Path(out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Validate every auto-EQ sub-bundle. All classes must declare
    # spectral_mask_eq with identical geometry so the runtime can dispatch
    # without per-class layout assertions.
    autoeq_metas: Dict[str, Dict[str, Any]] = {}
    canonical_sig: Optional[Tuple] = None
    canonical_cls: Optional[str] = None
    for cls, p in auto_eq_paths.items():
        m = _check_sub_bundle(p, expected_kind="nn+dsp",
                              expected_block_kind="spectral_mask_eq")
        blocks = m.get("dsp_blocks") or []
        p_ = blocks[0].get("params", {})
        sig = (
            p_.get("sample_rate"), p_.get("block_size"),
            p_.get("num_control_params"), p_.get("n_fft"),
            p_.get("hop"), p_.get("n_bands"),
            p_.get("min_gain_db"), p_.get("max_gain_db"),
            p_.get("f_min"), p_.get("f_max"),
        )
        if canonical_sig is None:
            canonical_sig = sig
            canonical_cls = cls
        elif sig != canonical_sig:
            raise ValueError(
                f"auto_eq class {cls!r} layout differs from "
                f"{canonical_cls!r}; classes must share geometry."
            )
        autoeq_metas[cls] = m

    sat_meta      = _check_sub_bundle(saturator_bundle, expected_kind="dsp",
                                      expected_block_kind="rational_a")
    ssl_comp_meta = _check_sub_bundle(ssl_comp_bundle,  expected_kind="nn")

    # Sample rate must be uniform across every stage of the chain.
    sample_rates = {sat_meta["sample_rate"], ssl_comp_meta["sample_rate"]}
    sample_rates.update(m["sample_rate"] for m in autoeq_metas.values())
    if len(sample_rates) != 1:
        raise ValueError(
            "sub-bundles disagree on sample_rate: "
            f"saturator={sat_meta['sample_rate']}, ssl_comp={ssl_comp_meta['sample_rate']}, "
            "auto_eq=" + ", ".join(f"{c}={m['sample_rate']}"
                                   for c, m in autoeq_metas.items())
        )
    sample_rate = sample_rates.pop()

    # Compose a stable model_id from the chain.
    autoeq_id = "_".join(autoeq_metas[c]["model_id"] for c in ordered)
    model_id = f"nm__{sat_meta['model_id']}__{autoeq_id}"

    # Copy sub-bundles into the staging dir under their stable role names.
    for role, src in (("saturator", saturator_bundle), ("ssl_comp", ssl_comp_bundle)):
        dst = out_dir / role
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
    for cls, src in auto_eq_paths.items():
        dst = out_dir / f"auto_eq_{cls}"
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)

    meta = _build_default_meta(model_id=model_id, sample_rate=int(sample_rate),
                               classes=ordered, default_class=default_class)
    meta = CompositePluginMeta(
        **{**asdict(meta), "effect_name": effect_name},
    )

    # Drift guard: never silently regenerate a different control set over an
    # existing bundle (see docstring). Key-set comparison only — ranges and
    # defaults may be tuned freely; adding/dropping a control id is what
    # breaks the meta<->C++ contract.
    meta_path = out_dir / "axon_meta.json"
    if meta_path.is_file() and not allow_control_set_change:
        try:
            old_controls = set(json.loads(meta_path.read_text())
                               .get("controls", {}).keys())
        except (OSError, ValueError):
            old_controls = None
        if old_controls is not None:
            new_controls = set(meta.controls.keys())
            if new_controls != old_controls:
                missing = sorted(old_controls - new_controls)
                added = sorted(new_controls - old_controls)
                raise ValueError(
                    "export would change the control set of the existing "
                    f"{meta_path} (would drop {missing}, would add {added}). "
                    "This usually means _build_default_meta drifted from the "
                    "C++ read-set. If the change is intentional, pass "
                    "allow_control_set_change=True and update "
                    "native/clap/src/axon_plugin.cpp + tests in lock-step."
                )

    meta_path.write_text(json.dumps(asdict(meta), indent=2) + "\n")
    return meta
