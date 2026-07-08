/* ============================================================================
 * Module: AUTO EQ (StageID 1)
 * ----------------------------------------------------------------------------
 * Control-only descriptor: the correction curve is drawn by the shell spectrum,
 * so this module owns no visualizer and no telemetry — just the parameter set.
 *   • CLS is an enum (class) control auto-rendered by AX.EnumSelect from meta.
 *   • EQ_ENGINE / EQ_RENDER / EQ_FREEZE are toggles with id-specific labels.
 * Ported 1:1 from the legacy PROCESSORS entry + toggleLabels for stage 1.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 1, name: 'AUTO EQ',
    params: ['CLS', 'EQ', 'EQR', 'EQB', 'EQS', 'EQ_ENGINE', 'EQ_RENDER', 'EQ_FREEZE'],
    wetParams: ['EQ'],
    labels: {
      EQ_ENGINE: ['NEURAL', 'ADAPTIVE'],
      EQ_RENDER: ['STFT', 'IIR'],
      EQ_FREEZE: ['LIVE', 'HOLD'],
    },
    // Channel-strip columns: target class, correction amount, adaptation speed,
    // and the engine/render/freeze mode toggles.
    groups: [
      { label: 'TARGET', params: ['CLS'] },
      { label: 'AMOUNT', params: ['EQ', 'EQR', 'EQB'] },
      { label: 'SPEED',  params: ['EQS'] },
      { label: 'MODE',   params: ['EQ_ENGINE', 'EQ_RENDER', 'EQ_FREEZE'] },
    ],
  });
})();
