# Decide the dormant Saturator's fate (delete vs revive)

Status: complete/implemented
Opened: 2026-07-06
Concluded: 2026-07-08
Outcome: Deleted the dormant Saturator stage + bundle (clean); byte-identical null; RationalA header kept.
Issue: #21

Owner decision with bundle-contract ripple — deliberately excluded from the
2026-07-06 cleanup pass (which deleted the Exciter).

## Why / evidence

- StageID 2 (Saturator, RationalA waveshaper) is dormant: not in
  processor_order, no UI tab, params kept. It was parked in June 2026 on a
  QUALITY stop-condition: the degree-6 rational nonlinearity at base rate
  ALIASES badly — alias/fundamental ≈ 3.86e-2 (−28 dB) vs 1.1e-4 (−79 dB) at
  8× oversampling, ~350× worse. Reviving it properly means adding ~4×
  oversampling (like the old exciter had), which adds latency — the original
  stop condition.
- Deleting it is NOT code-only (why the cleanup pass skipped it): the bundle
  contract requires the `saturator/` sub-bundle — `entry_init`'s
  `.at("saturator")`, `composite_meta.cpp` validation, `build.sh` sub-bundle
  checks, `weights/axon_bundle/saturator/`, composite.py's sub_bundles map,
  and the SDR/SVO/SMX/STH/SBS/SHF controls in the meta↔read-set contract.
- **`rational_a.hpp` must stay either way** — the SSL EQ's SEQ_DRIVE colour
  stage uses the RationalA waveshaper independently of the Saturator stage.

## Plan (whichever way the decision goes)

- DELETE: mirror the exciter-deletion recipe (enum value tombstoned, meta +
  composite.py + read-set in one commit, contract tests green, byte-identical
  golden renders) PLUS the bundle-format ripple above; bump anything that
  validates sub-bundle sets.
- REVIVE: 4× oversampled RationalA (polyphase up/down like the old exciter
  design), latency reported via CLAP, UI tab restored, alias measurement
  redone (target ≤ −70 dB alias/fund at full drive).

## Acceptance

- Delete path: suite + contract tests green, `uv run axon eval null`
  byte-identical (stage was dormant), bundle builds from weights/ without a
  saturator dir.
- Revive path: alias measurement, latency test, KAT for the oversampled
  path, owner listening.

## Results (2026-07-08)

Investigated the concrete footprint on `main` (post GUI-design-system merge).
No re-measurement of the alias figure was needed — the deciding factors are the
footprint and the (unchanged) quality stop-condition. Verdict below: **DELETE
the stage.**

### Dormancy — confirmed
- `StageID::Saturator = 2` is not in `kDefaultStageOrder` (`axon_plugin.cpp:123`
  comment says so explicitly); the new design-system GUI has no Saturator tab
  (no `ui/modules/saturator.js`). Removing it is byte-identical.

### Two findings that refine the plan
1. **The saturator bundle's trained RationalA is used ONLY by the dormant stage.**
   `sat_rational` (loaded from `weights/axon_bundle/saturator/plugin_meta.json`
   at `axon_plugin.cpp:2988`) feeds only `ch.saturator.reset(...)` (`:1248`). The
   SSL EQ never receives it: `SslChannelEq::set_harmonic()` is **defined but never
   called** anywhere. So deleting the stage + bundle is clean for the SSL EQ.
2. **SEQ_DRIVE "Colour" is currently a safe no-op** (separate incomplete feature).
   It's wired to `harmonic_mix` (`axon_plugin.cpp:1576`), but because
   `set_harmonic()` is never called, `rational_` stays empty and the colour guard
   `harm = hmix_>0 && !rational_.empty()` (`ssl_channel_eq.hpp:256`) keeps it
   bypassed. So the doc's premise ("SEQ_DRIVE colour uses RationalA") is aspirational,
   not active today. → filed as its own not-started idea (wire the colour); NOT in
   scope here.

`rational_a.hpp` (the header/type) stays either way: it's the member type of
`SslChannelEq` and the type of the saturator stage. Only the STAGE, its params,
and the bundle coeffs are in question. `true_peak_ceiling.hpp`'s saturator
reference is a comment only.

### Delete footprint (~10 files + bundle + export args)
- `axon_plugin.cpp`: tombstone `StageID::Saturator` (keep the slot, like the
  exciter); delete the `case StageID::Saturator` block (~:1840), the
  SDR/SVO/SMX/SHF/SLF/STH/SBS reads (~:1494), the `chain.saturator` member +
  `sat_hpf/lpf` state, and the `sat_meta`/`sat_rational` load (~:2950/:2988).
- `axon_stage_timing.h`: tombstone `AXON_ST_SATURATOR` + name-array slot.
- `composite_meta.cpp`: drop the `am.at("saturator")` validation (~:79).
- `composite.py`: remove the 7 `_ctl("S*")` saturator controls, the
  `sub_bundles["saturator"]`, `amount_mapping["saturator"]` (`_AmountMappingSat`),
  and the `_check_sub_bundle(saturator_bundle, ...)` call.
- `weights/axon_bundle/axon_meta.json`: remove the 7 `S*` controls +
  `sub_bundles.saturator` + `amount_mapping.saturator`.
- `weights/axon_bundle/saturator/` (12 KB: `plugin_meta.json` + hydra source):
  remove **or** relocate the coeffs (see the coeffs fork below).
- `scripts/export_axon.py`: make `--saturator-run/--saturator-bundle` optional (or
  drop them). `build.sh` sub-bundle discovery is generic (driven by meta) — no
  change beyond a comment. `install_axon_mac.sh`/`true_peak_ceiling.hpp`: comments.
- Contract/tests: keep `test_control_contract` green (it checks the SEQ_* set,
  not saturator); update any saturator-presence assertion; golden renders stay
  byte-identical (dormant).

### Coeffs fork (owner sub-decision)
The 12 KB trained RationalA is the intended source for wiring SEQ_DRIVE colour
(finding #2). So:
- **Delete-keep-coeffs (recommended):** remove the stage/params/DSP, but keep the
  tiny RationalA under a neutral role (rename `saturator`→`colour` in `sub_bundles`,
  or inline the coeffs) so the colour feature can be finished later without
  re-sourcing the model.
- **Delete-clean:** remove the bundle entirely; re-source coeffs if colour is ever
  wired.

### Verdict
**Delete the Saturator stage.** It's dormant, aliases badly at base rate
(alias/fund −28 dB vs −79 dB at 8× — ~350× worse), and reviving needs 4×
oversampling → added latency (the parked reason) plus UI restore + re-measure +
listening. The colour capability it was meant to provide is better served by
finishing the (separate) SSL-EQ SEQ_DRIVE colour path. Deletion is byte-identical,
follows the known exciter-deletion recipe, and sheds a whole sub-bundle + 7 params
+ a DSP stage + contract surface. Effort ~half a day; risk LOW–MEDIUM (bundle
ripple + export args need care).

## Outcome (2026-07-08)

**Decision: DELETE clean.** Removed the dormant Saturator stage (StageID 2) and
its sub-bundle entirely, mirroring the exciter-deletion recipe.

Shipped in one commit on `main`:
- **C++** (`axon_plugin.cpp`): tombstoned `StageID::Saturator` (value gap kept),
  deleted the stage `case`, the SDR/SVO/SMX/SHF/SLF/STH/SBS reads + AmountSnapshot
  fields, the `chain.saturator` member + sat HPF/LPF state, the `sat_meta`/
  `sat_rational` load, and the now-unused `RationalA`/`RationalAParams` usings +
  `rational_a.hpp` include. `axon_stage_timing.h`: tombstoned `AXON_ST_RETIRED_2`.
  `composite_meta.{hpp,cpp}`: dropped the dead `CompositeAmountSat`/`amt_sat`
  (parsed, never consumed) + its `amount_mapping.saturator` read.
- **Meta/bundle**: removed the 7 `S*` controls + `sub_bundles.saturator` +
  `amount_mapping.saturator` from `weights/axon_bundle/axon_meta.json`, and
  deleted `weights/axon_bundle/saturator/`. The shipped `model_id` was left
  unchanged so the installed plugin id stays stable.
- **Generators**: `axon/export/composite.py` + `scripts/export_axon.py` no longer
  emit/require the saturator (controls, sub_bundle, amount_mapping, CLI args,
  build steps); the composite `model_id` composition drops the sat prefix on the
  next re-export. `build.sh`/`install_axon_mac.sh`/`true_peak_ceiling.hpp` comments
  updated.
- **Tests**: updated `test_composite_meta.cpp` (now asserts saturator is ABSENT)
  and `test_composite_meta_validate.cpp` (dropped `amt_sat` asserts; a legacy
  `amount_mapping.saturator` in a fixture is now just a parser-tolerance check).
  `rational_a.hpp` and its `test_grey_dsp.cpp` RationalA tests stay — the
  waveshaper type is retained (member of `SslChannelEq`).

**Verification:** `uv run axon test` → 30/30 green. `uv run axon eval null --set all`
→ 3/3 sets **byte-identical** vs the pre-deletion bundle (2 ORT flakes resolved on
retry per the standard protocol) — confirming the stage was truly dormant. Bundle
builds from `weights/` with no saturator dir.

**Spun off:** the investigation found SEQ_DRIVE "Colour" is a wired-but-inert no-op
(`set_harmonic()` is never called) — filed as a new not-started idea.
