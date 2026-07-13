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
    help: {
      summary: 'Adds a decorrelated mastering ambience for depth and cohesion without moving the dry signal or its transients.',
      topics: [
        { title: 'Mix', body: 'Blends the reverb return with the dry program. Small moves are usually enough on a master.', groups: [0] },
        { title: 'Space', body: 'Size changes decay and density; Width controls how broadly the ambience opens around the mix.', groups: [1] },
        { title: 'Tone', body: 'Damping softens the tail above its cutoff; Low Cut keeps bass energy out of the wet path.', groups: [2] },
      ],
    },
  });
})();
