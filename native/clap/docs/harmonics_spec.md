# HARMONICS — two-knob dark-master rescue exciter

Replaces the old multi-knob Exciter surface (engine kept). Purpose: add
program-related harmonic content when a master is too dark and EQ isn't the
right fix. Opinionated, hard to misuse — two knobs, no Amount.

## Controls
| id | name | range | default | meaning |
|---|---|---|---|---|
| `EXC_ON` | Harmonics | switch | 1 | stage on/off |
| `EXC_WARM` | Warmth | 0–1 | 0 | even/2nd content from the low-mids → body |
| `EXC_PRES` | Presence | 0–1 | 0 | 2nd+3rd content from the highs → air |

Retired: `EXC_AMT, EXC_FREQ, EXC_DRIVE, EXC_CHAR, EXC_LPF, EXC_POLY` (the
short-lived RICH/CLEAN toggle is subsumed — HARMONICS is always CLEAN). Each
knob is the **added-harmonic level** for its band; the band freqs / drive /
character are fixed internally.

## Engine
Two `nablafx::Exciter` instances (CLEAN/polynomial shaper) in series onto an
untouched dry, reusing the proven anti-aliased (4×) band-limited parallel
topology, DC removal, delay-aligned dry, loudness-gentle blend:

| band | HPF | LPF | character | shaper | drive |
|---|---|---|---|---|---|
| **Warmth** | 100 Hz | 1 kHz | even (0.0) | CLEAN u² (pure 2nd) | 6 dB (fixed) |
| **Presence** | 3.5 kHz | 16.5 kHz | 0.5 | CLEAN u²+u³ (2nd+3rd) | 6 dB (fixed) |

- **Knob → level:** the knob (0–1) maps directly to its band's wet-blend amount
  (drive fixed → only level changes, not character). Mapping is currently
  **linear**; the taper/max is easily re-tuned by ear (see "measured range").
- **Latency:** each active band adds its FIR group delay (~32 samples); reported
  per active band (0 / 32 / 64), an inactive band early-returns with no delay.
- ≤3rd harmonic only (no fizz), mono-sum preserved, loudness-gentle — inherited.

## Visualization
Two fixed color-coded zones on the band display (amber **Warmth** 100 Hz–1 kHz,
cyan **Presence** 3.5–16.5 kHz; the 1–3.5 kHz mid gap is left untouched). Within
each zone a **live overlay** plots the actual added-harmonic energy from a
per-band wet-spectrum tap (`exc_warm_db` / `exc_pres_db`), so the glow is exactly
what each knob is adding *now* and breathes with the program. No draggable edges.

## Validation (harness_harmonics.cpp, SR 48k)
- **Bypass (0,0): bit-identical** (max|out−in| = 0).
- **Warmth on 400 Hz** → 2nd (800 Hz) rises monotonically: −63 (off) → −16.8
  (25%) → −10.8 (50%) → −4.7 dB (100%); cross-talk into the high 2nd is −68 dB.
- **Presence on 6 kHz** → 2nd/3rd rise monotonically (h2 −23→−10.4, h3 −34→−21
  over 25→100%); cross-talk into the low 2nd is −61 dB.
- **Measured range** = what "100%" means today: Warmth 100% ≈ −4.7 dB 2nd
  (strong), Presence 100% ≈ −10.4 dB. If "100% = too hot for mastering", cap the
  max blend or apply a gentler taper — a one-line change in the stage mapping.

## Status
Wired into `axon_plugin` (engine + meta + inject + latency + two wet taps), UI
(stage renamed HARMONICS, two knobs, two-zone overlay), packaged + installed.
Tunable taper is the obvious follow-up after a real-ear A/B.
