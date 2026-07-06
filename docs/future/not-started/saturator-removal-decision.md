# Decide the dormant Saturator's fate (delete vs revive)

Status: not-started
Opened: 2026-07-06
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
