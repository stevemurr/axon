# Adaptive EQ: frequency-dependent spectrum averaging (calmer high end)

Status: not-started
Opened: 2026-07-06
Issue: #15

Conditional follow-up to the 2026-07-05 wobble fixes: only worth doing if
listening still finds the high end "undulating" on real material with the
fixed engine (EQ_ENGINE=1).

## Why / evidence

- The wobble fixes pinned the running-spectrum EWMA at the validated 2 s,
  linked L/R, and rescaled smoothing sigmas — measured curve motion dropped
  ~7× at defaults (46.9 → ~6.6 dB/s on a spectrum-flipping torture signal;
  0.1–0.3 dB/s on steady material). That likely resolves the reported HF
  undulation; this doc is the next lever if residue remains.
- HF band energy is statistically burstier (cymbals/air come and go), so for
  equal estimator variance the top octaves need LONGER averaging than the
  mids — a single global tau under-smooths HF exactly where the ear notices.
- Zero-latency options (lookahead was evaluated and rejected: centered
  windowing of a 2 s average would cost ~1 s of real latency).

## Plan

1. `RunningMelSpectrum` (native/clap/src/adaptive_eq.hpp): per-band alpha
   array instead of a scalar — tau scaled from 2 s in the mids up to ~4–6 s
   above ~6 kHz (log-frequency ramp; the controller already has per-band
   attack/release scaffolding as precedent: a_att_/a_rel_).
2. Alternative worth an A/B: robust band statistics — track a running
   median/upper-percentile per band instead of mean power (kills burst
   sensitivity without slowing response to sustained change).
3. Measure with the wobble driver methodology (per-band dB/s + span on the
   torture signal AND cymbal-heavy program material), compare HF band motion
   before/after at equal mid-band responsiveness.

## Acceptance

- HF band (>6 kHz) curve motion reduced ≥2× on cymbal-heavy material with
  mid-band step response unchanged (±10%).
- `uv run axon test` green (adaptive controller unit tests may need per-band
  tau coverage); `uv run axon eval null` unaffected for EQ_ENGINE=0.
- Owner listening sign-off on the material that originally exposed it.
