/* ============================================================================
 * Module: EQ (StageID 3) — SSL 9000 J channel EQ
 * ----------------------------------------------------------------------------
 * CONTROL-ONLY module: a pure data descriptor, no visualizer or telemetry.
 * The frequency response is drawn by the shell's shared spectrum overlay.
 * Stereo, Mid, and Side each own a persistent manual-control bank. SEQ_MODE
 * selects which bank is visible for editing; all three run simultaneously.
 * SEQ_AUTO/SPLIT/CAL/RESET remain attached to the Stereo bank.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;

  const BANK_COLORS = ['#5CB0E8', '#35E0C8', '#F07BC6'];
  AX.eqBankColors = BANK_COLORS;
  const BANKS = [
    { name: 'Stereo', prefix: 'SEQ_',      color: BANK_COLORS[0] },
    { name: 'Mid',    prefix: 'SEQ_MID_',  color: BANK_COLORS[1] },
    { name: 'Side',   prefix: 'SEQ_SIDE_', color: BANK_COLORS[2] },
  ];
  const SUFFIXES = [
    'HPF_ON', 'HPF_F',
    'LF_G', 'LF_F', 'LF_BELL',
    'LMF_G', 'LMF_F', 'LMF_Q',
    'HMF_G', 'HMF_F', 'HMF_Q',
    'HF_G', 'HF_F', 'HF_BELL',
    'LPF_ON', 'LPF_F', 'DRIVE',
  ];
  const bankIndex = () => Math.max(0, Math.min(2, Math.round(AX.val('SEQ_MODE'))));
  const bankId = (suffix) => BANKS[bankIndex()].prefix + suffix;
  const labels = {
    SEQ_CAL: ['IDLE', 'CALIBRATE'],
    SEQ_RESET: ['IDLE', 'RESET'],
  };
  BANKS.forEach((b) => {
    labels[b.prefix + 'LF_BELL'] = ['SHELF', 'BELL'];
    labels[b.prefix + 'HF_BELL'] = ['SHELF', 'BELL'];
  });
  const allBankParams = [];
  BANKS.forEach((b) => SUFFIXES.forEach((s) => allBankParams.push(b.prefix + s)));

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 3, name: 'EQ',
    params: ['SEQ_ON', 'SEQ_MODE'].concat(allBankParams, ['SEQ_AUTO', 'SEQ_SPLIT', 'SEQ_CAL', 'SEQ_RESET']),
    wetParams: ['SEQ_ON'],
    labels,
    rebuildOn: ['SEQ_MODE'],
    stackSelect: { SEQ_MODE: { colors: BANK_COLORS } },
    dynAccent() { return BANKS[bankIndex()].color; },
    dynLabel(id) {
      const m = AX.state.meta[id];
      return m ? m.name.replace(/^(Mid|Side) /, '') : null;
    },
    // Channel-strip layout: each band is its own vertical column (shape/Q on
    // top, then freq, then gain), like a console EQ — instead of one long row.
    get groups() {
      const bank = BANKS[bankIndex()];
      return [
        { label: 'HPF',    params: [bankId('HPF_ON'), bankId('HPF_F')] },
        { label: 'LF',     params: [bankId('LF_BELL'), bankId('LF_F'), bankId('LF_G')] },
        { label: 'LMF',    params: [bankId('LMF_Q'), bankId('LMF_F'), bankId('LMF_G')] },
        { label: 'HMF',    params: [bankId('HMF_Q'), bankId('HMF_F'), bankId('HMF_G')] },
        { label: 'HF',     params: [bankId('HF_BELL'), bankId('HF_F'), bankId('HF_G')] },
        { label: 'LPF',    params: [bankId('LPF_ON'), bankId('LPF_F')] },
        { label: bank.name.toUpperCase(), params: ['SEQ_ON', 'SEQ_MODE', bankId('DRIVE')] },
        { label: 'ST ASSIST', params: ['SEQ_AUTO', 'SEQ_SPLIT'] },
        { label: 'ST SOLVE',  params: ['SEQ_CAL', 'SEQ_RESET'] },
      ];
    },
    help: {
      summary: 'Three simultaneous zero-latency EQ banks: independent Stereo, Mid, and Side tone shaping, with Auto EQ assist on Stereo.',
      topics: [
        { title: 'Filters', body: 'High-pass rumble and low-pass excess top end. Each section has a switch and cutoff.', groups: [0, 5] },
        { title: 'Tone bands', body: 'LF/HF switch between shelves and bells; LMF/HMF add variable-Q midrange control.', groups: [1, 2, 3, 4] },
        { title: 'Bank', body: 'Choose which persistent bank to edit. Stereo, Mid, and Side all remain active together; each curve has its own spectrum color.', groups: [6] },
        { title: 'Stereo assist', body: 'Share Auto EQ correction with the Stereo bank, then calibrate its visible gains or reset the solution.', groups: [7, 8] },
      ],
    },
  });
})();
