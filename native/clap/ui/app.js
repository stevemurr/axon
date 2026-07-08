/* ============================================================================
 * Axon — Application Shell (orchestration)
 * ----------------------------------------------------------------------------
 * Wires the design system into a running plugin UI: builds the chain strip and
 * control surface from the module registry, owns the two always-on visualizers
 * (spectrum + level meters) on the shared canvas toolkit with 60fps easing, and
 * implements the window.axon* bridge endpoints exactly as before. Loaded LAST,
 * after all system/*, registry.js and modules/*.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = window.AX;
  const T = AX.tokens;
  const $ = (id) => document.getElementById(id);

  /* ── DOM refs ──────────────────────────────────────────────────────────*/
  const chainEl = $('chain');
  const controlsEl = $('controls');
  const vizHost = $('viz');

  /* ── Control surface ───────────────────────────────────────────────────*/
  let widgets = {};        // id -> component (for host pushes)
  let cards = [];          // { desc, ctrl } for chain active/selected updates

  function onEdit(desc, id, v) {
    AX.bridge.sendParam(id, v);
    refreshChainActive();
    if (desc.rebuildOn && desc.rebuildOn.indexOf(id) >= 0) renderSurface();
    AX.loop.requestPaint();
  }

  function buildWidget(desc, id) {
    const m = AX.state.meta[id];
    if (!m) return null;
    const base = { meta: m, value: AX.val(id), accent: desc.accent };
    if (m.unit === 'enum' && Array.isArray(m.enumOptions) && m.enumOptions.length) {
      return AX.EnumSelect(Object.assign({}, base, { pretty: AX.prettyEnum, onInput: (v) => onEdit(desc, id, v) }));
    }
    if (m.unit === 'switch') {
      return AX.Toggle(Object.assign({}, base, { labels: AX.modules.toggleLabels(desc, id), label: m.name, onInput: (v) => onEdit(desc, id, v) }));
    }
    return AX.Knob(Object.assign({}, base, {
      label: AX.modules.knobLabel(desc, id),
      format: (v) => AX.modules.format(desc, id, v),
      onInput: (v) => onEdit(desc, id, v),
    }));
  }

  function hideAllViz() {
    document.querySelectorAll('#viz .viz-wrap').forEach((w) => w.classList.remove('is-shown'));
  }

  function addWidget(desc, id, parent) {
    const w = buildWidget(desc, id);
    if (w) { widgets[id] = w; parent.appendChild(w.el); }
  }

  function renderSurface() {
    const desc = AX.modules.get(AX.state.selected);
    controlsEl.innerHTML = '';
    widgets = {};
    // Layout modes:
    //  • grouped columns (default for grouped modules): band = a vertical column
    //    of stacked controls — the EQ channel-strip look.
    //  • grouped rows (grouped modules that ALSO have a visualizer): each group
    //    is a labelled horizontal cluster, so the controls stay one row tall and
    //    leave the graph its height.
    //  • flat: a wrapping row of controls.
    const grouped = !!(desc && desc.groups);
    const horiz = grouped && !!desc.visualizer;
    controlsEl.classList.toggle('is-grouped', grouped && !horiz);
    controlsEl.classList.toggle('is-grouped-row', horiz);
    AX.loop.setModule(null);
    hideAllViz();
    if (!desc) return;
    if (grouped) {
      for (const g of desc.groups) {
        const col = document.createElement('div');
        col.className = 'ax-group';
        if (g.label) { const h = document.createElement('div'); h.className = 'ax-group__label'; h.textContent = g.label; col.appendChild(h); }
        let host = col;
        if (horiz) { host = document.createElement('div'); host.className = 'ax-group__row'; col.appendChild(host); }
        for (const id of g.params) addWidget(desc, id, host);
        controlsEl.appendChild(col);
      }
    } else {
      for (const id of desc.params) addWidget(desc, id, controlsEl);
    }
    if (desc.visualizer) {
      desc.visualizer.build(vizHost);
      AX.loop.setModule(() => desc.visualizer.draw());
    }
  }

  /* ── Chain strip ───────────────────────────────────────────────────────*/
  let dragSrc = null;

  function renderChain() {
    chainEl.innerHTML = '';
    cards = [];
    AX.state.order.forEach((id, i) => {
      if (i > 0) { const a = document.createElement('span'); a.className = 'arrow'; a.textContent = '›'; chainEl.appendChild(a); }
      const desc = AX.modules.get(id);
      if (!desc) return;
      const ctrl = AX.Card({ name: desc.name, accent: desc.accent });
      ctrl.setSelected(id === AX.state.selected);
      ctrl.setActive(AX.modules.isActive(desc));
      const card = ctrl.el;
      card.draggable = true;
      card.dataset.index = i;
      card.addEventListener('click', () => { AX.state.selected = id; syncSelection(); renderSurface(); });
      card.addEventListener('dragstart', (e) => { dragSrc = i; e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', String(i)); card.classList.add('is-dragging'); });
      card.addEventListener('dragend', () => card.classList.remove('is-dragging'));
      card.addEventListener('dragover', (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; card.classList.add('is-drag-over'); });
      card.addEventListener('dragleave', () => card.classList.remove('is-drag-over'));
      card.addEventListener('drop', (e) => {
        e.preventDefault(); card.classList.remove('is-drag-over');
        const dest = parseInt(card.dataset.index, 10);
        if (dragSrc == null || dragSrc === dest) return;
        const order = AX.state.order.slice();
        const [moved] = order.splice(dragSrc, 1);
        order.splice(dest, 0, moved);
        AX.state.order = order; dragSrc = null;
        AX.bridge.sendOrder(order);
        renderChain();
      });
      chainEl.appendChild(card);
      cards.push({ id, desc, ctrl });
    });
  }
  function syncSelection() { for (const c of cards) c.ctrl.setSelected(c.id === AX.state.selected); }
  function refreshChainActive() { for (const c of cards) c.ctrl.setActive(AX.modules.isActive(c.desc)); }

  /* ── Panel toggles (Auto Gain / Bypass — meta switches, fixed placement)*/
  const PANEL_TOGGLES = [['auto-gain-btn', 'AGN'], ['bypass-btn', 'BYP']];
  function refreshPanelToggles() {
    for (const [bid, pid] of PANEL_TOGGLES) { const b = $(bid); if (b) b.classList.toggle('is-on', AX.val(pid) >= 0.5); }
  }
  PANEL_TOGGLES.forEach(([bid, pid]) => {
    const b = $(bid); if (!b) return;
    b.addEventListener('click', () => { const nv = AX.val(pid) >= 0.5 ? 0 : 1; AX.bridge.sendParam(pid, nv); refreshPanelToggles(); });
  });

  /* ══════════════════════════════════════════════════════════════════════
   *  SPECTRUM (always-on) — EQ curve/bins + SSL/total overlay, eased to 60fps
   * ════════════════════════════════════════════════════════════════════ */
  const spectrum = (function () {
    const canvas = $('spectrum-canvas');
    const W = 676, H = 148, CY = H / 2, DB_RANGE = 12;
    const FLO = 20, FHI = 20000, KDISP = 128, N_BINS = 50, DB_MIN = -90, DB_MAX = 0;
    const FS = AX.canvas.freq;
    const dispFreqs = Array.from({ length: KDISP }, (_, i) => FLO * Math.pow(FHI / FLO, i / (KDISP - 1)));
    const binFreqs = Array.from({ length: N_BINS }, (_, i) => FLO * Math.pow(FHI / FLO, i / (N_BINS - 1)));
    const EQ_BANDS = [
      { kind: 'low_shelf', fc: 1010, q: 0.707 }, { kind: 'peaking', fc: 110, q: 0.707 },
      { kind: 'peaking', fc: 1100, q: 0.707 }, { kind: 'peaking', fc: 7000, q: 0.707 },
      { kind: 'high_shelf', fc: 10000, q: 0.707 },
    ];
    const grid = new AX.canvas.StaticLayer();

    let mode = localStorage.getItem('eqViewMode') || 'bins';
    let raw = null;                                   // latest push (silhouette + order)
    let tGains = null, tBins = null, tSsl = null;     // targets
    const dGains = [0, 0, 0, 0, 0];
    const dBins = new Float32Array(N_BINS);
    const dSsl = new Float32Array(N_BINS);

    const toggleBtn = $('eq-view-toggle');
    const curveLbl = $('eq-view-curve'), binsLbl = $('eq-view-bins');
    curveLbl.classList.toggle('active', mode === 'curve');
    binsLbl.classList.toggle('active', mode === 'bins');
    toggleBtn.addEventListener('click', () => {
      mode = mode === 'curve' ? 'bins' : 'curve';
      localStorage.setItem('eqViewMode', mode);
      curveLbl.classList.toggle('active', mode === 'curve');
      binsLbl.classList.toggle('active', mode === 'bins');
      AX.loop.requestPaint();
    });

    const deltaToY = (db) => CY - Math.max(-1, Math.min(1, db / DB_RANGE)) * CY;
    const dbToY = (db) => H - ((Math.max(DB_MIN, Math.min(DB_MAX, db)) - DB_MIN) / (DB_MAX - DB_MIN)) * H;

    function drawGrid(g) {
      g.fillStyle = T.bgSunken; g.fillRect(0, 0, W, H);
      const rect = { x: 0, y: 0, w: W, h: H };
      AX.canvas.freqGrid(g, FS, rect, [50, 100, 200, 500, 1000, 2000, 5000, 10000]);
      g.strokeStyle = AX.rgba('#FFFFFF', 0.04);
      [-8, -4, 4, 8].forEach((db) => { const y = Math.round(deltaToY(db)) + 0.5; g.beginPath(); g.moveTo(0, y); g.lineTo(W, y); g.stroke(); });
      g.font = `8px ${T.fontUi}`; g.fillStyle = T.textMute; g.textAlign = 'center';
      [[100, '100'], [1000, '1k'], [10000, '10k']].forEach(([f, l]) => g.fillText(l, FS.toX(f, W), H - 14));
      g.textAlign = 'left';
      g.fillText('+8', 3, deltaToY(8) - 2); g.fillText('-8', 3, deltaToY(-8) + 9); g.fillText('0', 3, CY - 2);
      g.strokeStyle = AX.rgba(T.textDim, 0.35); g.beginPath(); g.moveTo(0, CY + 0.5); g.lineTo(W, CY + 0.5); g.stroke();
    }

    function curveDelta(gains) {
      return dispFreqs.map((f) => {
        let db = 0;
        for (let b = 0; b < 5; b++) { const c = AX.dsp.rbjCoeffs(EQ_BANDS[b].kind, EQ_BANDS[b].fc, EQ_BANDS[b].q, gains[b], 44100); db += AX.dsp.biquadResponse(f, 44100, c[0], c[1], c[2], c[3], c[4]); }
        return db;
      });
    }

    function draw() {
      const { ctx } = AX.canvas.fit(canvas, W, H);
      let moving = false;
      // Ease targets.
      if (tGains) for (let i = 0; i < 5; i++) { dGains[i] = AX.ease(dGains[i], tGains[i], 0.35); if (!AX.settled(dGains[i], tGains[i], 0.01)) moving = true; }
      if (tBins) for (let i = 0; i < N_BINS; i++) { dBins[i] = AX.ease(dBins[i], tBins[i], 0.35); if (!AX.settled(dBins[i], tBins[i], 0.02)) moving = true; }
      for (let i = 0; i < N_BINS; i++) { const tv = tSsl ? tSsl[i] : 0; dSsl[i] = AX.ease(dSsl[i], tv, 0.35); if (!AX.settled(dSsl[i], tv, 0.02)) moving = true; }

      grid.ensure(W, H, 'sg', drawGrid).blit(ctx);

      // Faint live pre-EQ magnitude silhouette (drawn from the raw push).
      if (raw && raw.db && raw.order) {
        const pos = raw.order.indexOf(1);
        if (pos > 0 && raw.db[pos - 1]) {
          ctx.save(); ctx.globalAlpha = 0.10; ctx.beginPath(); ctx.moveTo(0, H);
          for (let b = 0; b < KDISP; b++) ctx.lineTo(FS.toX(dispFreqs[b], W), dbToY(raw.db[pos - 1][b]));
          ctx.lineTo(W, H); ctx.closePath(); ctx.fillStyle = T.textDim; ctx.fill(); ctx.restore();
        }
      }

      toggleBtn.style.display = (raw && raw.eq_bins) ? 'block' : 'none';

      // EQ shape (bins or curve), split fill above/below centre + line.
      const haveShape = (mode === 'bins' && tBins) || (mode === 'curve' && tGains);
      if (haveShape) {
        const xs = (mode === 'bins') ? binFreqs.map((f) => FS.toX(f, W)) : dispFreqs.map((f) => FS.toX(f, W));
        const ys = (mode === 'bins') ? Array.from(dBins, (v) => deltaToY(v)) : curveDelta(dGains).map(deltaToY);
        const N = xs.length;
        const buildPath = () => { ctx.beginPath(); ctx.moveTo(xs[0], CY); for (let i = 0; i < N; i++) ctx.lineTo(xs[i], ys[i]); ctx.lineTo(xs[N - 1], CY); ctx.closePath(); };
        // Fills fade toward the extremes: strongest at the centre reference line.
        const gTop = ctx.createLinearGradient(0, 0, 0, CY);
        gTop.addColorStop(0, AX.rgba(T.accent, 0.04)); gTop.addColorStop(1, AX.rgba(T.accent, 0.5));
        const gBot = ctx.createLinearGradient(0, CY, 0, H);
        gBot.addColorStop(0, AX.rgba(T.danger, 0.46)); gBot.addColorStop(1, AX.rgba(T.danger, 0.03));
        ctx.save(); ctx.beginPath(); ctx.rect(0, 0, W, CY); ctx.clip(); buildPath(); ctx.fillStyle = gTop; ctx.fill(); ctx.restore();
        ctx.save(); ctx.beginPath(); ctx.rect(0, CY, W, H - CY); ctx.clip(); buildPath(); ctx.fillStyle = gBot; ctx.fill(); ctx.restore();
        // Centre line on top.
        ctx.save(); ctx.strokeStyle = AX.rgba(T.textDim, 0.7); ctx.beginPath(); ctx.moveTo(0, CY + 0.5); ctx.lineTo(W, CY + 0.5); ctx.stroke(); ctx.restore();
        // Auto-EQ line (its identity colour).
        ctx.save(); ctx.shadowBlur = 5; ctx.shadowColor = AX.rgba(AX.stageAccent(1), 0.8); ctx.strokeStyle = AX.stageAccent(1); ctx.lineWidth = 1.75; ctx.beginPath();
        for (let i = 0; i < N; i++) i === 0 ? ctx.moveTo(xs[i], ys[i]) : ctx.lineTo(xs[i], ys[i]); ctx.stroke(); ctx.restore();
        if (mode === 'bins') { ctx.save(); for (let i = 0; i < N; i++) { ctx.fillStyle = ys[i] <= CY ? AX.rgba(T.accent, 0.85) : AX.rgba(T.danger, 0.85); ctx.beginPath(); ctx.arc(xs[i], ys[i], 2, 0, Math.PI * 2); ctx.fill(); } ctx.restore(); }
      }

      // Overlay: SSL contribution (dashed) + TOTAL (Auto EQ + SSL).
      const haveSsl = !!tSsl;
      const haveAuto = !!tBins;
      if (haveSsl || haveAuto) {
        const bx = binFreqs.map((f) => FS.toX(f, W));
        const drawLine = (vals, color, width, dash, glow) => {
          ctx.save(); ctx.beginPath();
          for (let i = 0; i < N_BINS; i++) { const y = deltaToY(vals[i]); i === 0 ? ctx.moveTo(bx[i], y) : ctx.lineTo(bx[i], y); }
          ctx.strokeStyle = color; ctx.lineWidth = width; ctx.setLineDash(dash);
          if (glow) { ctx.shadowBlur = 6; ctx.shadowColor = glow; } ctx.stroke(); ctx.restore();
        };
        if (haveSsl) drawLine(dSsl, AX.stageAccent(3), 1.5, [5, 3], null);
        const tot = Array.from({ length: N_BINS }, (_, i) => (haveAuto ? dBins[i] : 0) + (haveSsl ? dSsl[i] : 0));
        if (haveSsl || haveAuto) drawLine(tot, T.text, 2, [], AX.rgba(T.text, 0.3));
        AX.canvas.legend(ctx, [
          { color: T.text, label: 'TOTAL' },
          { color: AX.stageAccent(1), label: 'AUTO EQ' },
          { color: haveSsl ? AX.stageAccent(3) : AX.rgba(AX.stageAccent(3), 0.4), label: 'EQ', dashed: true },
        ], 6, 8);
      }
      return moving;
    }

    window.axonSpectrum = function (data) { raw = data; if (data && data.eq) tGains = data.eq; if (data && data.eq_bins) tBins = data.eq_bins; AX.loop.requestPaint(); };
    window.axonSslCurve = function (d) { tSsl = (d && d.on) ? d.bins : null; AX.loop.requestPaint(); };
    window.axonRenderSpectrum = function () { AX.loop.requestPaint(); };
    return { draw };
  })();
  AX.loop.add(spectrum.draw);

  /* ══════════════════════════════════════════════════════════════════════
   *  LEVEL METERS (always-on) — IN/OUT, LUFS/RMS/PEAK, eased bars
   * ════════════════════════════════════════════════════════════════════ */
  (function () {
    const canvas = $('meter-canvas');
    const W = 104, H = 360;
    const MODES = {
      lufs: { field: 'lufs_s', min: -36, max: 0, step: 6 },
      rms: { field: 'rms', min: -48, max: 0, step: 6 },
      peak: { field: 'peak', min: -48, max: 0, step: 6 },
    };
    const TGT_LO = -14, TGT_HI = -11;
    const PAD_T = 12, PAD_B = 26, PAD_L = 26, PAD_R = 6;
    const barTop = PAD_T, barBot = H - PAD_B, barH = barBot - barTop;
    const areaL = PAD_L, areaR = W - PAD_R, areaW = areaR - areaL;
    const gap = 10, barW = (areaW - gap) / 2;
    const grid = new AX.canvas.StaticLayer();

    let mode = localStorage.getItem('meterMode') || 'lufs';
    let target = null;
    const disp = { in: -120, out: -120 };

    const yFor = (db, m) => barTop + (m.max - Math.max(m.min, Math.min(m.max, db))) / (m.max - m.min) * barH;
    function fillColor(db) {
      if (mode === 'lufs') { if (db >= TGT_LO && db <= TGT_HI) return T.ok; if (db > TGT_HI) return T.danger; return AX.rgba(T.ok, 0.45); }
      if (db >= -1) return T.danger; if (db >= -6) return T.warn; return T.accent;
    }

    function drawGrid(g) {
      const m = MODES[mode];
      g.clearRect(0, 0, W, H);
      g.font = `10px ${T.fontUi}`; g.textAlign = 'right'; g.textBaseline = 'middle';
      for (let db = m.max; db >= m.min; db -= m.step) {
        const y = yFor(db, m);
        g.strokeStyle = AX.rgba('#FFFFFF', 0.06); g.beginPath(); g.moveTo(areaL, y); g.lineTo(areaR, y); g.stroke();
        g.fillStyle = T.textMute; g.fillText(String(db), areaL - 4, y);
      }
      if (mode === 'lufs') {
        const yHi = yFor(TGT_HI, m), yLo = yFor(TGT_LO, m);
        g.fillStyle = AX.rgba(T.ok, 0.16); g.fillRect(areaL, yHi, areaW, yLo - yHi);
        g.strokeStyle = AX.rgba(T.ok, 0.5); g.setLineDash([4, 3]);
        g.beginPath(); g.moveTo(areaL, yHi); g.lineTo(areaR, yHi); g.stroke();
        g.beginPath(); g.moveTo(areaL, yLo); g.lineTo(areaR, yLo); g.stroke(); g.setLineDash([]);
      }
    }

    function bar(ctx, x, db, label, m) {
      ctx.fillStyle = AX.rgba('#FFFFFF', 0.05); ctx.fillRect(x, barTop, barW, barH);
      if (db > m.min) { const y = yFor(db, m); ctx.fillStyle = fillColor(db); ctx.fillRect(x, y, barW, barBot - y); }
      ctx.strokeStyle = T.border; ctx.lineWidth = 1; ctx.strokeRect(x + 0.5, barTop + 0.5, barW - 1, barH - 1);
      ctx.fillStyle = T.textDim; ctx.font = `700 12px ${T.fontUi}`; ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
      ctx.fillText(label, x + barW / 2, H - 9);
    }

    function draw() {
      const { ctx } = AX.canvas.fit(canvas, W, H);
      const m = MODES[mode];
      const tIn = (target && target.in) ? target.in[m.field] : m.min;
      const tOut = (target && target.out) ? target.out[m.field] : m.min;
      disp.in = AX.ease(disp.in, tIn, 0.4); disp.out = AX.ease(disp.out, tOut, 0.4);
      const moving = !AX.settled(disp.in, tIn, 0.05) || !AX.settled(disp.out, tOut, 0.05);

      ctx.clearRect(0, 0, W, H);
      grid.ensure(W, H, 'mg-' + mode, drawGrid).blit(ctx);
      bar(ctx, areaL, disp.in, 'IN', m);
      bar(ctx, areaL + barW + gap, disp.out, 'OUT', m);

      const inZone = (v) => mode === 'lufs' && v >= TGT_LO && v <= TGT_HI;
      const fmt = (v) => (v <= m.min + 0.01 || v <= -119) ? '–∞' : v.toFixed(1);
      const elI = $('meter-in-val'), elO = $('meter-out-val');
      elI.textContent = fmt(disp.in); elO.textContent = fmt(disp.out);
      elI.classList.toggle('in-zone', inZone(disp.in)); elO.classList.toggle('in-zone', inZone(disp.out));
      return moving;
    }

    document.querySelectorAll('.meter-mode').forEach((btn) => {
      btn.classList.toggle('is-on', btn.dataset.mode === mode);
      btn.addEventListener('click', () => {
        mode = btn.dataset.mode; localStorage.setItem('meterMode', mode);
        document.querySelectorAll('.meter-mode').forEach((b) => b.classList.toggle('is-on', b.dataset.mode === mode));
        AX.loop.requestPaint();
      });
    });

    window.axonMeters = function (data) { target = data; AX.loop.requestPaint(); };
    AX.loop.add(draw);
  })();

  /* ── Install module telemetry endpoints (limiter/buscomp/…) ────────────*/
  function installTelemetry() {
    for (const desc of AX.modules.list()) {
      if (!desc.telemetry) continue;
      for (const key in desc.telemetry) {
        const fn = desc.telemetry[key];
        window[key] = (d) => { fn(d); AX.loop.requestPaint(); };
      }
    }
  }
  installTelemetry();

  /* ── Bridge: init + host param pushes ──────────────────────────────────*/
  window.axonInit = function (state) {
    AX.state.meta = state.paramMeta || {};
    AX.state.values = state.paramValues || {};
    const visible = new Set(AX.modules.ids());
    AX.state.order = (state.processorOrder || AX.modules.ids()).filter((id) => visible.has(id));
    // Ensure any registered module missing from the pushed order is still shown.
    for (const id of AX.modules.ids()) if (AX.state.order.indexOf(id) < 0) AX.state.order.push(id);
    AX.state.selected = AX.state.order[0] != null ? AX.state.order[0] : null;
    renderChain();
    renderSurface();
    refreshPanelToggles();
    AX.loop.start();
    AX.loop.requestPaint();
  };

  window.axonSetParam = function (id, value) {
    AX.state.values[id] = value;
    if (widgets[id]) widgets[id].update(value);
    if (id === 'AGN' || id === 'BYP') refreshPanelToggles();
    const desc = AX.modules.get(AX.state.selected);
    if (desc && desc.rebuildOn && desc.rebuildOn.indexOf(id) >= 0) renderSurface();
    refreshChainActive();
    AX.loop.requestPaint();
  };

  // Dev/gallery fallback: if we're not inside the host, start the loop so the
  // empty grids render. The host calls axonInit() which also starts it.
  if (!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.axon)) {
    AX.loop.start();
  }
})();
