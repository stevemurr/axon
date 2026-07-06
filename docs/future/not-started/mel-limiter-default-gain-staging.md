# MelLimiter: make the ballistics knobs audible out of the box

Status: not-started
Opened: 2026-07-06
Issue: #17

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
