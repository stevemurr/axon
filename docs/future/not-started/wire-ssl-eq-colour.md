# Wire the SSL-EQ SEQ_DRIVE "Colour" (set_harmonic is never called)

Status: not-started
Opened: 2026-07-08
Issue: #27

Discovered while investigating the Saturator removal (#21).

## Why / evidence

- SEQ_DRIVE ("Colour", 0..1) IS wired to the SSL EQ's harmonic stage:
  `e.harmonic_mix = sdrv` (`axon_plugin.cpp:1576`). So turning it up *should*
  blend in the RationalA waveshaper: `x = (1-hmix)*x + hmix*rational_.eval(x)`
  (`ssl_channel_eq.hpp:260`).
- But `SslChannelEq::set_harmonic()` (`ssl_channel_eq.hpp:164`) is **defined and
  never called** anywhere in the codebase. So `rational_` stays default (empty),
  and the guard `const bool harm = hmix_ > 0.0 && !rational_.empty()`
  (`ssl_channel_eq.hpp:256`) keeps the colour bypassed regardless of SEQ_DRIVE.
- Net: **SEQ_DRIVE "Colour" is a safe no-op today** — a wired-but-inert control.
  (Verified: a default `RationalA` has empty num/den, so `eval(x)=0`; only the
  `!rational_.empty()` guard prevents that 0 from silencing the signal.)
- The trained RationalA coeffs that were meant to drive it lived in the saturator
  sub-bundle, which was **deleted** in #21. So wiring the colour now needs a
  coefficient source.

## Plan

- Choose a coeff source:
  - (a) Re-export a small RationalA "colour" sub-bundle (e.g. `sub_bundles.colour`)
    and load it at activate — the same shape the deleted saturator bundle had
    (12 KB `plugin_meta.json`, `kind: rational_a`).
  - (b) Inline a curated `num`/`den` set as a constant in `ssl_channel_eq.hpp`
    (no bundle dependency).
- Call `ssl_eq.set_harmonic(num, den)` at chain construction (the way the old
  `ch.saturator.reset(...)` did) so `harm` flips on when `SEQ_DRIVE > 0`.
- Address aliasing: the degree-6 RationalA aliases badly at base rate (the reason
  the Saturator stage was parked — see #21). A *colour blend* at modest mix may be
  acceptable without oversampling, but MEASURE alias/fundamental at full SEQ_DRIVE
  and decide whether a light oversample (adds latency) is warranted.

## Acceptance

- With `SEQ_DRIVE > 0`, output changes (adds the intended harmonic colour) and
  `SEQ_DRIVE = 0` stays bit-identical bypass (`uv run axon eval null` at drive 0).
- Alias/fundamental measured + documented at representative drive settings.
- Unit test asserting the waveshaper is engaged (`harm == true`) when coeffs are
  loaded and `hmix > 0`; whole suite green.
