/* ============================================================================
 * Module: REVERB (StageID 8)
 * ----------------------------------------------------------------------------
 * Control-only stage: mix, size, width, damping, and a low-cut on the wet path.
 * No visualizer, labels, or telemetry — a pure data descriptor. RVB_MIX drives
 * the chain "active" dot.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  AX.registerModule({
    id: 8, name: 'REVERB',
    params: ['RVB_MIX', 'RVB_SIZE', 'RVB_WIDTH', 'RVB_DAMP', 'RVB_LOWCUT'],
    wetParams: ['RVB_MIX'],
    // Channel-strip columns: wet mix, the space (size + width), and the tone
    // shaping (damping + low-cut on the wet path).
    groups: [
      { label: 'MIX',   params: ['RVB_MIX'] },
      { label: 'SPACE', params: ['RVB_SIZE', 'RVB_WIDTH'] },
      { label: 'TONE',  params: ['RVB_DAMP', 'RVB_LOWCUT'] },
    ],
  });
})();
