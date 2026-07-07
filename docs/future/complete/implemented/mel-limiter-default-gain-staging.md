# MelLimiter: make the ballistics knobs audible out of the box

Status: complete/implemented
Opened: 2026-07-06
Issue: #17
Concluded: 2026-07-06
Outcome: Shipped — MelLimiter Drive default 2 → 5 dB so the Attack/Release
knobs are audibly responsive out of the box (dead −35 dBFS → responsive
−23 dBFS); ceiling + ballistics code unchanged.

Settings change (plus one optional coefficient) — not a bug fix. The user
reported the Attack/Release knobs "do nothing"; investigation proved the
plumbing is fully intact and the knobs work as designed, but at default
Drive 2 dB / Ceiling 0 dBFS the limiter does ~1 dB of gain reduction, so
there is nothing for ballistics to shape.

## Why / evidence (measured 2026-07-05)

- Defaults on the real fixture: brick GR −0.96 dB, band GR 0. At hard drive
  (10 dB) the knobs measurably work: DYN attack 0→1 moves brick GR −11.0 →
  −6.3 dB with 0 → 0.21% clipped samples (texture at constant loudness);
  release 0→1 swings recovery 99 → >900 ms.
- Even mode ignores the knobs for the brickwall BY DESIGN (extremes diff at
  low drive: literally zero, −180 dB).
- Lookahead was tested and rejected: doubling kBrickLA to 512 did not reduce
  loose-mode clipping (0.21% → 0.24%) because `atk_samps` is a multiple of
  kBrickLA (self-scaling); band-path lookahead covers only ~1/3 of the
  16–22 ms solve ramp for +256 samples latency. Full data in the 2026-07-05
  session record.

## Plan

1. Defaults in `weights/axon_bundle/axon_meta.json` (+ composite.py in
   lockstep — the contract test enforces): MLD (Drive) 2.0 → 4–6 dB and/or
   MLC (Ceiling) 0.0 → −1.0. OWNER SOUND DECISION: pick by ear.
2. Optional polish: cap the loose-attack coefficient in `mel_limiter.cpp`
   (`0.15 + adaptive_gain*1.05` → `*0.80`) so max Attack stays ≤ the
   lookahead window — a *softer* attack without hard-clip grit, and a knob
   identity distinct from the water-fill blend (MLG is currently
   dual-purpose).

## Acceptance

- Owner listening test: knobs audibly change behavior at defaults.
- `uv run axon test` green (limiter KATs may need updating if the attack
  coefficient changes — that is an intentional ballistics change, not drift).
- `uv run axon eval null` is EXPECTED to fail vs old defaults (that's the
  point) — record before/after renders instead.

## Results (2026-07-06)

Reconfirmed by ear + measurement (isolated MelLimiter, Dynamic brickwall
`MLA=1`, auto-gain off; ATTACK knob `MLG` tight vs loose, max sample diff):

| Drive / Ceiling | attack tight→loose Δ | verdict |
|---|---|---|
| 2 dB / 0 dBFS (stock) | **−35 dBFS** | knob effectively dead |
| 5 dB / 0 dBFS | **−23 dBFS** | knob responsive |
| 6 dB / −3 dBFS | −18.5 dBFS | more, but still moderate |

At stock the limiter does ~1 dB GR, so the ballistics have nothing to shape —
the reported "knobs do nothing." **Drive is the effective lever** (Ceiling at 0
never binds on this material — peaks land at −3..−5 dBFS, so lowering it only
changes character, not knob responsiveness). The effect is subtle-by-nature (a
mel-band limiter, not a slamming brickwall): even pushed hard the attack knob
stays moderate, so a *dramatic* response would need aggressive defaults or a
ballistics-range code change — rejected as wrong for a master-bus default (the
true-peak ceiling stage + LUFS auto-gain already own final loudness/safety).

Verdict: the smallest fix — Drive default 2 → 5 dB, Ceiling unchanged —
makes Attack/Release audibly responsive without turning the stage into a
slammer. The optional attack-coefficient cap was skipped (it changes knob
*character*, not the dead-knob complaint).

## Outcome

Shipped 2026-07-06 on branch `fix/mel-limiter-default-drive`. **MLD (Drive)
default 2.0 → 5.0 dB** in BOTH the generator (`axon/export/composite.py:154`)
and the shipped artifact (`weights/axon_bundle/axon_meta.json`), kept in
lockstep (`test_composite_contract` asserts generator↔meta default equality).
Ceiling and all ballistics code unchanged.

Verification:
- `axon/export/test_composite_contract.py`: 2 passed (generator ↔ meta defaults agree).
- `uv run axon test`: 30/30 green — limiter KATs test the DSP directly, so they
  are unaffected by a meta-default change (no attack-coefficient change was made).
- End-to-end: the DEFAULT preset render is now **byte-identical to explicit
  Drive 5** (−inf dBFS) and differs from the old Drive 2 default by −14.6 dBFS —
  the new default is in effect through the whole load→process path.
- `eval null` vs old defaults intentionally differs (the default sound changed
  by design); the before/after knob-responsiveness (−35 → −23 dBFS) is the
  recorded evidence.
