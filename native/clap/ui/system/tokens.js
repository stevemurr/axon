/* ============================================================================
 * Axon Design System — Design Tokens (JS mirror of tokens.css)
 * ----------------------------------------------------------------------------
 * window.AX.tokens holds the SAME palette/scale as tokens.css so the SVG knobs
 * and <canvas> painters read the identical values the DOM does — no more hex
 * literals re-typed per painter. This file is loaded FIRST and establishes the
 * window.AX namespace. Keep hex values byte-identical to tokens.css; the gallery
 * renders both side-by-side as a parity eyeball.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = (window.AX = window.AX || {});

  const t = {
    // Base surfaces
    bg:            '#0B0D10',
    bgSunken:      '#070809',
    surface:       '#14171C',
    surfaceRaised: '#1B1F26',
    surfaceHover:  '#212632',

    // Structure
    border:        '#232830',
    borderStrong:  '#2E353F',
    hairline:       'rgba(255,255,255,0.055)',
    hairlineStrong: 'rgba(255,255,255,0.10)',

    // Text
    text:     '#E6EAF0',
    textDim:  '#8A94A3',
    textMute: '#566072',

    // Accent
    accent:     '#35E0C8',
    accentDim:  'rgba(53,224,200,0.55)',
    accentWeak: 'rgba(53,224,200,0.12)',
    accentGlow: 'rgba(53,224,200,0.35)',
    accentInk:  '#04140F',

    // Semantic
    danger:     '#FF6B57',
    dangerWeak: 'rgba(255,107,87,0.14)',
    warn:       '#F3B14E',
    ok:         '#4FD08A',
    okWeak:     'rgba(79,208,138,0.15)',

    // Type
    fontUi:   "-apple-system, system-ui, 'Segoe UI', Roboto, 'Helvetica Neue', sans-serif",
    fontMono: "ui-monospace, 'SF Mono', 'Roboto Mono', Menlo, Consolas, monospace",

    // Motion
    durFast: 120,
    durMed:  200,
  };

  // Per-stage identity accents, indexed by StageID. Gaps (0, 7) are retired
  // slots kept so the array stays StageID-indexed — matches C++ StageID enum.
  const stage = {
    0: t.textMute,
    1: '#EAC15F', // AUTO EQ   — gold
    2: '#FF9F45', // SAT       — amber (dormant)
    3: '#5CB0E8', // SSL EQ    — sky
    4: '#B58BFF', // BUS COMP  — violet
    5: '#FF6B57', // LIMITER   — coral
    6: '#7FD46A', // BASS MONO — green
    7: '#F9C74F', // (retired — Exciter; slot kept)
    8: '#46B6E0', // REVERB    — blue
    9: '#CE9BF0', // WIDENER   — lavender
  };

  // Spacing (px numbers, for canvas layout math).
  const sp = { 1: 4, 2: 8, 3: 12, 4: 16, 5: 20, 6: 24, 7: 28, 8: 32 };

  /* --- helpers ---------------------------------------------------------- */

  // rgba() from a "#RRGGBB" (or existing rgba/hsla) string + alpha. Lets a
  // painter tint any token colour without hardcoding a literal:
  //   AX.rgba(AX.tokens.accent, 0.5)
  function rgba(color, alpha) {
    if (typeof color !== 'string') return color;
    if (color[0] !== '#') {
      // already rgba()/hsla() — best-effort swap of its alpha
      return color.replace(/[\d.]+\s*\)$/, alpha + ')');
    }
    let h = color.slice(1);
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    const n = parseInt(h, 16);
    const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
    return `rgba(${r},${g},${b},${alpha})`;
  }

  // Activation colour ramp: idle accent → warn → danger as `a` goes 0→1.
  // Used by meters and the limiter viz for "how hard is this working".
  function heat(a) {
    a = Math.max(0, Math.min(1, a));
    // interpolate hue teal(168)→red(8), keep it luminous on the dark bg
    const hue = 168 - a * 160;
    const sat = 60 + a * 22;
    const lig = 55 + a * 4;
    return `hsl(${hue.toFixed(0)},${sat.toFixed(0)}%,${lig.toFixed(0)}%)`;
  }

  AX.tokens = t;
  AX.tokens.stage = stage;
  AX.tokens.sp = sp;
  AX.stageAccent = (id) => stage[id] || t.accent;
  AX.rgba = rgba;
  AX.heat = heat;
})();
