/* ============================================================================
 * Module: BASS MONO (StageID 6)
 * ----------------------------------------------------------------------------
 * Control-only stage: collapses the stereo image to mono below a cutoff to
 * tighten the low end. No visualizer or telemetry — the descriptor is pure data
 * (intensity + crossover frequency), rendered by the shared control surface.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 6, name: 'BASS MONO',
    params: ['BMI', 'BMF'],
    wetParams: ['BMI'],
    // Single labelled column: enable on top, crossover frequency below.
    groups: [
      { label: 'MONO', params: ['BMI', 'BMF'] },
    ],
    help: {
      summary: 'Tightens the low end by removing stereo side information below a crossover while leaving the mono sum unchanged.',
      topics: [
        { title: 'Enable', body: 'Switch the low-frequency mono fold on or off. The mid signal always remains untouched.', groups: [0] },
        { title: 'Frequency', body: 'Sets the crossover: lows below it become mono while stereo width above it is preserved.', groups: [0] },
      ],
    },
  });
})();
