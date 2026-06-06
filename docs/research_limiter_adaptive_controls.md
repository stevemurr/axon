# Research: Do the MelLimiter adaptive controls (MLG / MLS / MLA) do anything audible?

Investigation only — no production code changed. Measurement probe: `/tmp/probe.cpp`
(built against `native/clap/src/mel_limiter.{hpp,cpp}` with `-framework Accelerate`).

## TL;DR

All three controls are **fully wired and measurably audible** — none is a no-op.
The reason MLG/MLS feel hard to "locate" is that **each control does two different
jobs depending on the MLA (Dynamic) toggle**, and one of those jobs (MLG's
brickwall-attack character in Dynamic mode) changes *distortion/grit*, not level —
which is exactly the kind of thing that's hard to hear on a knob sweep with no
visual feedback. Concrete numbers below.

---

## 1. WIRED?  (exact entry points)

Param flow: UI id → `axon_plugin.cpp:1207-1212` (`AmountSnapshot`) →
`axon_plugin.cpp:1552-1557` (`MelLimiter::Params`) → DSP.

### MLG — "Adaptive Gain" / (Dynamic) "Attack"  → `Params::adaptive_gain`
Wired in **two** places, gated by MLA:

- **Spectral solver (always on)** — `mel_limiter.cpp:193,198`:
  ```
  const float alpha = p.adaptive_gain;
  out_gains[n] = clamp((1-alpha)*g_uni + alpha*g_wf, 0, 1);
  ```
  Blends the uniform gain (`C/total`, transparent) with the per-band
  reverse-water-filling gain (`min(1, λ/L[n])`, multiband/louder). α=0 → every
  band reduced equally; α=1 → only the loud bands ducked.
- **Brickwall attack character (only when MLA on)** — `mel_limiter.cpp:286-289`:
  ```
  atk_samps = adaptive_brickwall ? kBrickLA*(0.15 + adaptive_gain*1.05)  // 38..307 smp
                                 : kBrickLA*0.25;                         // fixed 64 smp
  ```
  Tight (38 smp, fully pre-ducks inside the 256-smp lookahead → clean) ↔ loose
  (307 smp, slower than the lookahead so transients leak to the hard safety clip
  → punch + clipper grit).

### MLS — "Adaptive Speed" / (Dynamic) "Release"  → `Params::adaptive_speed`
Also wired in two places:

- **Spectral band-gain release (always on)** — `mel_limiter.cpp:270`:
  `rel_ms = 30 + adaptive_speed*370` → 30…400 ms one-pole release on the per-band gains.
- **Brickwall release (only when MLA on)** — `mel_limiter.cpp:283-284`:
  `rel_ms = adaptive_brickwall ? 50 + adaptive_speed*350 : 50` → 50…400 ms.

### MLA — "Dynamic" switch  → `Params::adaptive_brickwall`
Wired at `mel_limiter.cpp:283,286`. It is a **router**: off = fixed tight/fast
brickwall (MLG/MLS only touch the spectral stage); on = MLG/MLS *additionally*
reshape the brickwall attack/release.

**No no-ops, nothing double-applied incorrectly.** The "double duty" is by design
(documented in `docs/limiter_algorithm.md` §8). One honest caveat: because MLG/MLS
each drive both the spectral and brickwall stages simultaneously, their effect is
*coupled* and not cleanly attributable to one mechanism by ear.

---

## 2. AUDIBLE?  (measured)

Signal: 220 Hz pad + 1760 Hz layer + periodic broadband transient bursts
(input crest 22.7 dB), Drive +18 dB, Ceiling −1 dBFS — driven hard so both stages
engage. RMS swings reported across each knob's full 0→1 range, other knobs fixed.
Output peak is pinned at 0.891 (−1 dBFS) in every case — the ceiling always holds.

| Sweep | Mode | RMS change (0→1) | Other effect |
|---|---|---|---|
| **A. MLG** | Dynamic OFF (spectral α) | **−2.94 dB**, crest 6.9→9.8 dB | THD 0.52→0.73 (tone shift) |
| **B. MLS** | Dynamic OFF (spectral release) | **−2.57 dB**, crest 6.9→9.4 dB | breathing/pumping |
| **C. MLG** | Dynamic ON (brick attack) | **−0.20 dB** (small) | **THD 0.51→0.69** — grit/punch, not level |
| **D. MLS** | Dynamic ON (brick release) | **−6.84 dB**, crest 6.6→13.4 dB | strong pumping |
| **E. MLA** | off vs on (G=S=0.5) | **−2.98 dB** | crest 8.2→11.1 dB |

Brickwall GR-release time, isolated transient (section F): MLS measurably stretches
recovery — recover-to-90 % goes ~116 ms → ~520 ms → ~920 ms as MLS 0→0.5→1.

Spectral band-gain spread (section G), MLG=0 vs 1: at α=0 all 26 bands sit at the
*same* gain (−9.1 dB, spread 0 dB); at α=1 the spread is **13 dB** (loud bands
−16 dB, quiet bands −3 dB). This is the multiband action made concrete.

**Verdict.** These are *not* subtle and *not* broken. Three of the four mechanisms
move output by 2.5–6.8 dB and change crest by 3–7 dB — clearly audible. The one
exception is the headline confusion:

> **MLG in Dynamic mode (the "Attack" knob) barely changes level (0.2 dB) — it
> changes harmonic distortion (THD 0.51→0.69).** On a loud master with no GR meter,
> a 0.2 dB level move with a grit change is genuinely hard to localize by ear.

So the user "can't hear it" mostly because (a) Dynamic-mode Attack is a
distortion/punch control masquerading next to level controls, and (b) MLG/MLS each
do two coupled things, so a sweep doesn't isolate a single sensation. It is
working as intended; it just isn't *legible*.

Important condition: **everything here only happens while the limiter is actually
in gain reduction.** With low Drive / high Ceiling the spectral solver early-outs
(`mel_limiter.cpp:165`, `total ≤ C`) and the brickwall gain stays at 1 — then
MLG/MLS/MLA do nothing at all. If the user audited them with insufficient Drive,
they would correctly hear no difference.

---

## 3. VISUALIZE?

What `drawLimViz()` already shows (`ui/index.html:1816-1915`): per-band input
levels as bars colored by GR, a dashed Ceiling line, frequency ticks, and a single
**PEAK GR** mini-bar + number for the brickwall (`_lim.brick`). It is a *spectral
snapshot* — it shows the result of the water-filling solve, but **nothing
time-domain**, so the attack/release knobs (the whole point of MLG/MLS) are
invisible. There is no envelope, so the user has no way to *see* a release getting
slower.

Options, ranked value/effort:

1. **GR-over-time strip (best ROI).** Add a small scrolling history of brickwall
   GR (and optionally total spectral GR) next to/under the existing canvas. MLS
   release becomes obvious (the trace's tail visibly lengthens); MLA off/on and
   MLG attack show as differently-shaped dips. Cheap: `_lim.brick` is already
   streamed every frame — just keep a ring buffer and `lineTo` it. ~30 lines, no
   DSP change. **Directly visualizes exactly the knobs in question.**
2. **Attack/release envelope overlay on the knobs.** When Dynamic is on, draw a
   tiny attack-ramp / release-tail glyph that morphs with MLG/MLS (like a
   compressor's A/R curve). Pure UI math from the documented formulas
   (`atk=kBrickLA*(0.15+g*1.05)`, `rel=50+s*350 ms`). Medium effort, makes the
   *intent* legible even before audio plays.
3. **Static transfer curve.** Lowest value here — a brickwall is hard-knee at the
   ceiling, so the curve barely changes with MLG/MLS; it wouldn't teach the user
   anything the bands already show. Skip.

Option 1 is the honest fix for "I can't locate what they're doing": the controls
are time-domain, so they need a time-domain display.

---

## 4. COLLAPSE?

The controls are genuinely doing distinct things, so blunt removal loses behavior.
But there is real redundancy/coupling worth simplifying:

- **MLG and MLS each drive two stages at once.** That coupling is what makes them
  feel vague. It is *not* harmful, but it means neither knob maps to one sensation.
- **MLG-in-Dynamic-mode (Attack) is the weakest, hardest-to-hear control** (0.2 dB
  level; it's a grit/punch trim). It's the prime candidate to fold away.

Reasonable consolidations (in order of how confidently I'd recommend them):

- **A single "Character / Punch" macro is viable.** MLG and MLS are positively
  correlated in feel (both add density/pumping as they rise) and both already
  re-route through MLA. A one-knob macro that scales α and release together (e.g.
  0→transparent/fast, 1→multiband/slow-punchy) would cover most of the musically
  useful space with far less confusion. Keep MLA as the only extra switch
  (Even↔Dynamic). This is the strongest simplification.
- **Auto-couple brickwall speed to Drive** instead of exposing MLS twice: more
  Drive → slightly slower release, which is the usual "the harder you push, the
  more it breathes" intuition. Removes a knob but hides behavior; only do this if
  the goal is a simpler product, not a engineer's tool.
- **Do NOT** silently drop MLA — it's a clean, audible mode switch (−3 dB / +3 dB
  crest at the same settings) and users understand Even-vs-Dynamic.

I would *not* crowbar a macro in without the visualization first — see recommendation.

---

## 5. RECOMMENDATION

The controls work; the problem is **legibility, not efficacy**. In priority order:

1. **Add the GR-over-time strip (Visualize option 1).** Highest value, ~30 lines,
   no DSP change, and it makes MLS/MLA/Dynamic immediately visible. This alone
   likely resolves the user's "I can't locate it" complaint, because the effect is
   time-domain and currently has zero time-domain display.
2. **Then decide on collapse.** Once you can *see* the controls, you can judge
   whether the 6-knob spread is still worth it. If you want fewer knobs, the
   "Character/Punch" macro (collapse §4) is the clean choice — but make that call
   after the meter exists, not before.
3. **Leave the DSP alone.** Nothing here is broken, no-op, or double-applied in
   error. The one genuinely faint control (MLG-as-Attack, 0.2 dB) is a
   distortion/punch trim by design; if anything, surface it via the THD/grit it
   creates (the GR strip's clip activity) rather than expecting it to move a level
   meter.

Honest bottom line: **don't rip anything out, and don't add a macro yet — add the
time visualization.** That is the smallest change that turns "I can't hear it" into
"oh, *that's* what it does."

---

### Probe reproduction
```
g++ -O2 -std=c++17 -I native/clap/src /tmp/probe.cpp native/clap/src/mel_limiter.cpp \
    -framework Accelerate -o /tmp/probe && /tmp/probe
```
