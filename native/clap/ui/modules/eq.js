/* ============================================================================
 * Module: EQ (StageID 3) — SSL 9000 J channel EQ
 * ----------------------------------------------------------------------------
 * CONTROL-ONLY module: a pure data descriptor, no visualizer or telemetry.
 * The frequency response is drawn by the shell's shared spectrum overlay.
 * LF/HF swap shelf<->bell via *_BELL; LMF/HMF carry Q; HPF/LPF have on/off +
 * cutoff. SEQ_AUTO/SPLIT/CAL/RESET drive the Auto-EQ coupling (assist bands
 * absorb the Auto-EQ correction — CAL solves it).
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 3, name: 'EQ',
    params: [
      'SEQ_ON',
      'SEQ_HPF_ON', 'SEQ_HPF_F',
      'SEQ_LF_G', 'SEQ_LF_F', 'SEQ_LF_BELL',
      'SEQ_LMF_G', 'SEQ_LMF_F', 'SEQ_LMF_Q',
      'SEQ_HMF_G', 'SEQ_HMF_F', 'SEQ_HMF_Q',
      'SEQ_HF_G', 'SEQ_HF_F', 'SEQ_HF_BELL',
      'SEQ_LPF_ON', 'SEQ_LPF_F',
      'SEQ_DRIVE',
      'SEQ_AUTO', 'SEQ_SPLIT', 'SEQ_CAL', 'SEQ_RESET',
    ],
    wetParams: ['SEQ_ON'],
    labels: {
      SEQ_LF_BELL: ['SHELF', 'BELL'],
      SEQ_HF_BELL: ['SHELF', 'BELL'],
      SEQ_CAL: ['IDLE', 'CALIBRATE'],
      SEQ_RESET: ['IDLE', 'RESET'],
    },
    // Channel-strip layout: each band is its own vertical column (shape/Q on
    // top, then freq, then gain), like a console EQ — instead of one long row.
    groups: [
      { label: 'HPF',    params: ['SEQ_HPF_ON', 'SEQ_HPF_F'] },
      { label: 'LF',     params: ['SEQ_LF_BELL', 'SEQ_LF_F', 'SEQ_LF_G'] },
      { label: 'LMF',    params: ['SEQ_LMF_Q', 'SEQ_LMF_F', 'SEQ_LMF_G'] },
      { label: 'HMF',    params: ['SEQ_HMF_Q', 'SEQ_HMF_F', 'SEQ_HMF_G'] },
      { label: 'HF',     params: ['SEQ_HF_BELL', 'SEQ_HF_F', 'SEQ_HF_G'] },
      { label: 'LPF',    params: ['SEQ_LPF_ON', 'SEQ_LPF_F'] },
      { label: 'MAIN',   params: ['SEQ_ON', 'SEQ_DRIVE'] },
      { label: 'ASSIST', params: ['SEQ_AUTO', 'SEQ_SPLIT'] },
      { label: 'SOLVE',  params: ['SEQ_CAL', 'SEQ_RESET'] },
    ],
  });
})();
