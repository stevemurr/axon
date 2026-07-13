/* ============================================================================
 * Module: LIMITER (StageID 5) — MelLimiter
 * ----------------------------------------------------------------------------
 * EXEMPLAR module: shows the full pattern the design system expects —
 *   • a data-only descriptor (params, labels, dynamic relabel, rebuildOn)
 *   • an optional visualizer built on AX.canvas (HiDPI, StaticLayer grid)
 *   • telemetry endpoints that accumulate signal state and let the rAF loop
 *     ease displayed values to 60fps.
 * Ported 1:1 from the legacy drawLimViz / drawLimGR / window.axonLimiter, with
 * per-frame easing replacing the per-push EMA so the bands glide.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX, T = AX.tokens;

  /* ── Signal state (accumulates even when the module isn't selected) ─────*/
  const HIST_N = 180;                         // ~8.5 s at ~21 fps
  const grHist = new Float32Array(HIST_N);    // brick (peak) GR, dB ≤ 0
  const grBandHist = new Float32Array(HIST_N);// deepest spectral-band GR, dB ≤ 0
  let head = 0, len = 0;
  function pushHist(brick, band) { grHist[head] = brick; grBandHist[head] = band; head = (head + 1) % HIST_N; if (len < HIST_N) len++; }

  let raw = null;                             // latest push
  let dLvl = null, dGr = null;                // eased display arrays

  // Activation → colour: teal (idle) → amber → red (heavy GR).
  function grColor(grDb, alpha) {
    const a = Math.max(0, Math.min(1, -grDb / 9));
    return `hsla(${(168 - a * 158).toFixed(0)}, ${(55 + a * 25).toFixed(0)}%, ${(52 + a * 6).toFixed(0)}%, ${alpha})`;
  }

  /* ── Visualizer DOM ────────────────────────────────────────────────────*/
  const gridGr = new AX.canvas.StaticLayer();
  let built = false, cvGr, cvViz;
  function build(host) {
    if (!built) {
      const wg = document.createElement('div'); wg.className = 'viz-wrap'; wg.id = 'lim-gr-wrap';
      cvGr = document.createElement('canvas'); wg.appendChild(cvGr); host.appendChild(wg);
      const wv = document.createElement('div'); wv.className = 'viz-wrap'; wv.id = 'lim-viz-wrap';
      cvViz = document.createElement('canvas'); wv.appendChild(cvViz); host.appendChild(wv);
      built = true;
    }
    cvGr.parentElement.classList.add('is-shown');
    cvViz.parentElement.classList.add('is-shown');
  }

  /* ── Band level + gain-reduction display (eased) ───────────────────────*/
  function drawViz() {
    const W = 660, H = 96;
    const { ctx } = AX.canvas.fit(cvViz, W, H);
    ctx.clearRect(0, 0, W, H);
    const LV_TOP = 6, LV_BOT = -60, padL = 22, padB = 12, padT = 14;
    const plotL = padL, plotR = W - 6, plotW = plotR - plotL, plotT = padT, plotB = H - padB, plotH = plotB - plotT;
    const yFor = (db) => plotT + (LV_TOP - Math.max(LV_BOT, Math.min(LV_TOP, db))) / (LV_TOP - LV_BOT) * plotH;

    ctx.font = `8px ${T.fontUi}`; ctx.textBaseline = 'middle';
    for (let db = LV_TOP; db >= LV_BOT; db -= 12) {
      const y = yFor(db); ctx.strokeStyle = AX.rgba('#FFFFFF', 0.05); ctx.beginPath(); ctx.moveTo(plotL, y); ctx.lineTo(plotR, y); ctx.stroke();
      ctx.fillStyle = T.textMute; ctx.textAlign = 'right'; ctx.fillText(String(db), plotL - 3, y);
    }

    let moving = false;
    if (raw && raw.lvl) {
      const N = raw.lvl.length;
      if (!dLvl || dLvl.length !== N) { dLvl = raw.lvl.slice(); dGr = (raw.gr || new Array(N).fill(0)).slice(); }
      for (let i = 0; i < N; i++) {
        const tl = raw.lvl[i]; dLvl[i] = AX.ease(dLvl[i], tl, tl > dLvl[i] ? 0.5 : 0.18); if (!AX.settled(dLvl[i], tl, 0.05)) moving = true;
        const tg = raw.gr ? raw.gr[i] : 0; dGr[i] = AX.ease(dGr[i], tg, tg < dGr[i] ? 0.55 : 0.14); if (!AX.settled(dGr[i], tg, 0.05)) moving = true;
      }
      const slot = plotW / N, barW = slot * 0.74;
      if (typeof raw.ceiling === 'number') {
        const yc = yFor(raw.ceiling); ctx.strokeStyle = AX.rgba(T.accent, 0.55); ctx.setLineDash([4, 3]);
        ctx.beginPath(); ctx.moveTo(plotL, yc); ctx.lineTo(plotR, yc); ctx.stroke(); ctx.setLineDash([]);
        ctx.fillStyle = AX.rgba(T.accent, 0.8); ctx.textAlign = 'left'; ctx.fillText('CEIL', plotL + 2, yc - 6 < plotT ? yc + 6 : yc - 6);
      }
      const active = !raw || raw.active !== false;
      ctx.globalAlpha = active ? 1 : 0.3;
      for (let b = 0; b < N; b++) {
        const lvl = dLvl[b], gr = dGr[b], x = plotL + b * slot + (slot - barW) / 2, yLvl = yFor(lvl), yOut = yFor(lvl + gr);
        if (plotB - yLvl > 0) { ctx.fillStyle = grColor(gr, 0.92); ctx.fillRect(x, yLvl, barW, plotB - yLvl); }
        if (gr < -0.3 && yOut - yLvl > 0.5) { ctx.fillStyle = grColor(gr, 1); ctx.fillRect(x, yLvl, barW, Math.min(yOut - yLvl, 3)); }
      }
      ctx.globalAlpha = 1;
      if (raw.f) {
        ctx.fillStyle = T.textMute; ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
        [100, 1000, 10000].forEach((tf) => { let bi = 0, bd = 1e9; for (let b = 0; b < N; b++) { const d = Math.abs(raw.f[b] - tf); if (d < bd) { bd = d; bi = b; } } ctx.fillText(tf >= 1000 ? (tf / 1000) + 'k' : String(tf), plotL + bi * slot + slot / 2, H - 3); });
      }
      if (!active) { ctx.fillStyle = T.textDim; ctx.font = `700 11px ${T.fontUi}`; ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.fillText('LIMITER OFF', W / 2, H / 2); }
    }
    ctx.font = `8px ${T.fontUi}`; ctx.textAlign = 'right'; ctx.textBaseline = 'top';
    ctx.fillStyle = grColor(0, 1); ctx.fillText('■', plotR - 60, 2);
    ctx.fillStyle = grColor(-9, 1); ctx.fillText('■', plotR - 52, 2);
    ctx.fillStyle = T.textDim; ctx.fillText('idle → limiting', plotR, 2);
    return moving;
  }

  /* ── Scrolling gain-reduction-over-time strip ──────────────────────────*/
  function drawGr() {
    const W = 660, H = 66;
    const { ctx } = AX.canvas.fit(cvGr, W, H);
    ctx.clearRect(0, 0, W, H);
    const GR_BOT = -12, padL = 22, padR = 30, padT = 11, padB = 13;
    const plotL = padL, plotR = W - padR, plotW = plotR - plotL, plotT = padT, plotB = H - padB, plotH = plotB - plotT;
    const yFor = (db) => plotT + (0 - Math.max(GR_BOT, Math.min(0, db))) / (0 - GR_BOT) * plotH;

    ctx.font = `8px ${T.fontUi}`; ctx.textBaseline = 'middle';
    [0, -3, -6, -12].forEach((db) => { const y = yFor(db); ctx.strokeStyle = AX.rgba('#FFFFFF', 0.05); ctx.beginPath(); ctx.moveTo(plotL, y); ctx.lineTo(plotR, y); ctx.stroke(); ctx.fillStyle = T.textMute; ctx.textAlign = 'right'; ctx.fillText(String(db), plotL - 3, y); });
    ctx.fillStyle = T.textDim; ctx.textAlign = 'left'; ctx.textBaseline = 'top'; ctx.fillText('GAIN REDUCTION → time', plotL + 2, 1);

    if (len > 1) {
      const start = (head - len + HIST_N) % HIST_N;
      const xFor = (k) => plotR - ((len - 1 - k) / (HIST_N - 1)) * plotW;
      ctx.beginPath(); ctx.moveTo(xFor(0), plotT);
      for (let k = 0; k < len; k++) ctx.lineTo(xFor(k), yFor(grHist[(start + k) % HIST_N]));
      ctx.lineTo(xFor(len - 1), plotT); ctx.closePath(); ctx.fillStyle = AX.rgba(T.danger, 0.20); ctx.fill();
      ctx.beginPath();
      for (let k = 0; k < len; k++) { const x = xFor(k), y = yFor(grBandHist[(start + k) % HIST_N]); k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
      ctx.lineWidth = 1; ctx.strokeStyle = AX.rgba(T.accent, 0.45); ctx.stroke();
      ctx.beginPath();
      for (let k = 0; k < len; k++) { const x = xFor(k), y = yFor(grHist[(start + k) % HIST_N]); k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
      ctx.lineWidth = 1.5; ctx.strokeStyle = AX.rgba(T.danger, 0.95); ctx.stroke();
    }
    const cur = len ? grHist[(head - 1 + HIST_N) % HIST_N] : 0;
    ctx.fillStyle = T.textDim; ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(`${cur <= -0.05 ? cur.toFixed(1) : '0.0'}`, plotR + 3, yFor(Math.max(GR_BOT, cur)));
    ctx.font = `8px ${T.fontUi}`; ctx.textBaseline = 'bottom'; ctx.textAlign = 'right';
    ctx.fillStyle = AX.rgba(T.danger, 0.95); ctx.fillText('peak', plotR - 34, H - 2);
    ctx.fillStyle = AX.rgba(T.accent, 0.7); ctx.fillText('band', plotR, H - 2);
  }

  /* ── Register ──────────────────────────────────────────────────────────*/
  AX.registerModule({
    id: 5, name: 'LIMITER',
    params: ['MLD', 'MLC', 'MLG', 'MLS', 'MLA'],
    wetParams: ['MLI'],
    labels: { MLA: ['EVEN', 'DYNAMIC'] },
    // Labelled horizontal clusters (this module carries visualizers, so controls
    // stay one row tall): drive/ceiling, then the adaptive/dynamic section.
    groups: [
      { label: 'OUTPUT',   params: ['MLD', 'MLC'] },
      { label: 'ADAPTIVE', params: ['MLA', 'MLG', 'MLS'] },
    ],
    help: {
      summary: 'A 26-band perceptual limiter that redistributes gain reduction across the spectrum, followed by a lookahead peak safety stage.',
      topics: [
        { title: 'Output', body: 'Drive pushes level into the limiter; Ceiling sets the highest permitted output sample.', groups: [0] },
        { title: 'Adaptive', body: 'Choose even or dynamic peak control, then shape spectral adaptation and its timing.', groups: [1] },
        { title: 'Meters', body: 'The upper strip shows gain reduction over time; the band display reveals where limiting occurs.', visualizers: ['lim-gr-wrap', 'lim-viz-wrap'] },
      ],
    },
    rebuildOn: ['MLA'],
    dynLabel: (id) => {
      const dyn = AX.val('MLA') >= 0.5;
      if (dyn && id === 'MLG') return 'Attack';
      if (dyn && id === 'MLS') return 'Release';
      return null;
    },
    visualizer: { build, draw() { const m = drawViz(); drawGr(); return m; } },
    telemetry: {
      axonLimiter(d) {
        raw = d;
        let band = 0;
        if (d && d.gr) for (let i = 0; i < d.gr.length; i++) if (d.gr[i] < band) band = d.gr[i];
        pushHist((d && typeof d.brick === 'number') ? d.brick : 0, band);
      },
    },
  });
})();
