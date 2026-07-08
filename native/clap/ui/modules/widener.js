/* ============================================================================
 * Module: WIDENER (StageID 9) — stereo widener
 * ----------------------------------------------------------------------------
 * Control-only stage: an on/off enable plus Amount/Freq/Air knobs. No live
 * visualizer or telemetry, so this is a pure data descriptor. WID_ON drives the
 * chain "active" dot via wetParams; the toggle uses the default OFF/ON labels.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  AX.registerModule({
    id: 9, name: 'WIDENER',
    params: ['WID_ON', 'WID_AMT', 'WID_FREQ', 'WID_AIR'],
    wetParams: ['WID_ON'],
    // Channel-strip columns: the width section (enable + amount + crossover) and
    // the high-frequency air.
    groups: [
      { label: 'WIDTH', params: ['WID_ON', 'WID_AMT', 'WID_FREQ'] },
      { label: 'AIR',   params: ['WID_AIR'] },
    ],
  });
})();
