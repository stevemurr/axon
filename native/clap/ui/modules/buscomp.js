/* ============================================================================
 * Module: BUS COMP (StageID 4) — SSL-style bus compressor
 * ----------------------------------------------------------------------------
 * Ported 1:1 from the legacy drawBusCompCrunch / window.axonBusComp "BUS COMP
 * DISTORTION STRIP": a scrolling 180-sample history of program distortion
 * (1−coherence, dB ≤ 0) with a faint secondary crest-reduction "comp" line.
 * History is pushed RAW per telemetry frame (no easing) — the strip only
 * advances on a new push, so draw() returns false and repaints when the native
 * axonBusComp endpoint requests a paint. Structurally identical to the limiter
 * GR strip; only colours (→ tokens) and canvas setup (→ AX.canvas.fit) changed.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX, T = AX.tokens;

  /* ── Signal state (accumulates even when the module isn't selected) ─────*/
  const HIST_N = 180;
  const bcHist = new Float32Array(HIST_N);      // distortion level, dB ≤ 0
  const bcCrestHist = new Float32Array(HIST_N); // crest reduction, dB ≥ 0
  let head = 0, len = 0;
  function pushHist(distDb, crestDb) { bcHist[head] = distDb; bcCrestHist[head] = crestDb; head = (head + 1) % HIST_N; if (len < HIST_N) len++; }

  /* ── Visualizer DOM ────────────────────────────────────────────────────*/
  let built = false, cv;
  function build(host) {
    if (!built) {
      const w = document.createElement('div'); w.className = 'viz-wrap'; w.id = 'bc-crunch-wrap';
      cv = document.createElement('canvas'); w.appendChild(cv); host.appendChild(w);
      built = true;
    }
    cv.parentElement.classList.add('is-shown');
  }

  /* ── Scrolling distortion-over-time strip ──────────────────────────────*/
  function drawCrunch() {
    const W = 660, H = 176;
    const { ctx } = AX.canvas.fit(cv, W, H);
    ctx.clearRect(0, 0, W, H);

    // Primary axis: distortion (1−coherence) level, 0 dB (top) → -48 dB (bottom).
    const R_TOP = 0, R_BOT = -48;
    // Secondary implied scale for crest reduction "comp": 0 (bottom) → +12 (top).
    const C_TOP = 12, C_BOT = 0;
    const padL = 22, padR = 30, padT = 11, padB = 13;
    const plotL = padL, plotR = W - padR, plotW = plotR - plotL;
    const plotT = padT, plotB = H - padB, plotH = plotB - plotT;
    const yResid = (db) => plotT + (R_TOP - Math.max(R_BOT, Math.min(R_TOP, db))) / (R_TOP - R_BOT) * plotH;
    const yCrest = (db) => plotB - (Math.max(C_BOT, Math.min(C_TOP, db)) - C_BOT) / (C_TOP - C_BOT) * plotH;

    // Distortion dB grid + labels (left axis: 0 at top → -48 at bottom).
    ctx.font = `8px ${T.fontUi}`; ctx.textBaseline = 'middle';
    [0, -12, -24, -36, -48].forEach((db) => {
      const y = yResid(db);
      ctx.strokeStyle = AX.rgba('#FFFFFF', 0.05); ctx.beginPath(); ctx.moveTo(plotL, y); ctx.lineTo(plotR, y); ctx.stroke();
      ctx.fillStyle = T.textMute; ctx.textAlign = 'right'; ctx.fillText(String(db), plotL - 3, y);
    });

    ctx.fillStyle = T.textDim; ctx.textAlign = 'left'; ctx.textBaseline = 'top';
    ctx.fillText('DISTORTION (1−coherence) → time', plotL + 2, 1);

    if (len > 1) {
      const start = (head - len + HIST_N) % HIST_N;
      const xFor = (k) => plotR - ((len - 1 - k) / (HIST_N - 1)) * plotW; // newest → right

      // Distortion — filled area from the bottom up to the curve (primary).
      ctx.beginPath();
      ctx.moveTo(xFor(0), plotB);
      for (let k = 0; k < len; k++) ctx.lineTo(xFor(k), yResid(bcHist[(start + k) % HIST_N]));
      ctx.lineTo(xFor(len - 1), plotB);
      ctx.closePath();
      ctx.fillStyle = AX.rgba(AX.stageAccent(4), 0.22); ctx.fill();

      // Crest reduction "comp" — faint secondary line on its own implied scale.
      ctx.beginPath();
      for (let k = 0; k < len; k++) { const x = xFor(k), y = yCrest(bcCrestHist[(start + k) % HIST_N]); k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
      ctx.lineWidth = 1; ctx.strokeStyle = AX.rgba(AX.stageAccent(3), 0.45); ctx.stroke();

      // Distortion line on top, brighter (the headline).
      ctx.beginPath();
      for (let k = 0; k < len; k++) { const x = xFor(k), y = yResid(bcHist[(start + k) % HIST_N]); k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
      ctx.lineWidth = 1.5; ctx.strokeStyle = AX.rgba(AX.stageAccent(4), 0.95); ctx.stroke();
    }

    // Current distortion readout at the right edge.
    const cur = len ? bcHist[(head - 1 + HIST_N) % HIST_N] : -48;
    ctx.fillStyle = T.textDim; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    ctx.fillText(`${cur.toFixed(1)}`, plotR + 3, yResid(Math.max(R_BOT, cur)));

    // Line legend.
    ctx.font = `8px ${T.fontUi}`; ctx.textBaseline = 'bottom'; ctx.textAlign = 'right';
    ctx.fillStyle = AX.rgba(AX.stageAccent(4), 0.95); ctx.fillText('distort', plotR - 30, H - 2);
    ctx.fillStyle = AX.rgba(AX.stageAccent(3), 0.7); ctx.fillText('comp', plotR, H - 2);
  }

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 4, name: 'BUS COMP',
    params: ['SSC', 'SSC_IN'],
    wetParams: ['SSC'],
    // Labelled horizontal cluster (this module carries a visualizer, so controls
    // stay one row tall).
    groups: [
      { label: 'COMP', params: ['SSC', 'SSC_IN'] },
    ],
    visualizer: { build, draw() { drawCrunch(); return false; } },
    telemetry: {
      axonBusComp(d) {
        // Record RAW (unsmoothed) distortion + crest-reduction for the strip.
        const dist = (d && typeof d.distortion === 'number') ? d.distortion : -48;
        const crest = (d && typeof d.crest === 'number') ? d.crest : 0;
        pushHist(dist, crest);
      },
    },
  });
})();
