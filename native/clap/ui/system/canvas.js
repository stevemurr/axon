/* ============================================================================
 * Axon Design System — Canvas Toolkit (AX.canvas + AX.dsp)
 * ----------------------------------------------------------------------------
 * One implementation of the drawing primitives every visualizer used to
 * re-invent: HiDPI sizing, log-frequency + dB coordinate scales, grid / axis /
 * legend renderers, and a StaticLayer offscreen cache so a grid is drawn ONCE
 * and blitted each frame (the core "render fast" lever). Colours come from
 * AX.tokens — painters never type a hex literal. AX.dsp holds the exact,
 * C++-matched magnitude-response math the EQ / filter curves depend on.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = (window.AX = window.AX || {});
  const T = AX.tokens;

  /* ── HiDPI fit ─────────────────────────────────────────────────────────
   * Sizes the backing store to logical*dpr and scales the context so every
   * painter draws in crisp logical pixels. Pass explicit logical W/H (the meter
   * historically used a 2× backing store — be explicit, don't infer). */
  function fit(canvas, logicalW, logicalH) {
    const dpr = Math.max(1, window.devicePixelRatio || 1);
    const lw = logicalW != null ? logicalW : (canvas._lw || canvas.width);
    const lh = logicalH != null ? logicalH : (canvas._lh || canvas.height);
    canvas._lw = lw; canvas._lh = lh;
    canvas.style.width = lw + 'px';
    canvas.style.height = lh + 'px';
    const bw = Math.round(lw * dpr), bh = Math.round(lh * dpr);
    if (canvas.width !== bw)  canvas.width = bw;
    if (canvas.height !== bh) canvas.height = bh;
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, W: lw, H: lh, dpr };
  }

  function clear(ctx, W, H, fill) {
    if (fill) { ctx.fillStyle = fill; ctx.fillRect(0, 0, W, H); }
    else ctx.clearRect(0, 0, W, H);
  }

  /* ── Log-frequency scale ───────────────────────────────────────────────
   * Matches the C++/legacy mapping exactly: x = log(f/fLo)/log(fHi/fLo) * W. */
  function makeFreqScale(fLo, fHi) {
    const L = Math.log(fHi / fLo);
    return {
      fLo, fHi,
      toX: (f, W) => (Math.log(f / fLo) / L) * W,
      toFreq: (x, W) => fLo * Math.exp((x / W) * L),
    };
  }

  /* ── dB scale over a plot rect ─────────────────────────────────────────
   * dbTop maps to y=plotT, dbBot maps to y=plotT+plotH (top-down). */
  function makeDbScale(dbTop, dbBot) {
    const span = dbTop - dbBot;
    return {
      dbTop, dbBot,
      toY: (db, plotT, plotH) =>
        plotT + (dbTop - Math.max(dbBot, Math.min(dbTop, db))) / span * plotH,
    };
  }

  /* ── Vertical frequency grid (+ optional decade labels) ────────────────*/
  function freqGrid(ctx, scale, rect, freqs, opts) {
    opts = opts || {};
    ctx.save();
    ctx.lineWidth = 1;
    ctx.setLineDash([]);
    ctx.strokeStyle = opts.line || T.hairline;
    for (const f of freqs) {
      const x = Math.round(rect.x + scale.toX(f, rect.w)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, rect.y);
      ctx.lineTo(x, rect.y + rect.h);
      ctx.stroke();
    }
    if (opts.labels) {
      ctx.fillStyle = opts.labelColor || T.textMute;
      ctx.font = `${opts.labelSize || 8}px ${T.fontUi}`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'alphabetic';
      for (const [f, lbl] of opts.labels) {
        ctx.fillText(lbl, rect.x + scale.toX(f, rect.w), rect.y + rect.h - (opts.labelPad || 3));
      }
    }
    ctx.restore();
  }

  /* ── Horizontal dB grid (+ left-edge value labels) ─────────────────────*/
  function dbGrid(ctx, dbScale, rect, dbs, opts) {
    opts = opts || {};
    ctx.save();
    ctx.lineWidth = 1;
    ctx.setLineDash([]);
    ctx.font = `${opts.labelSize || 8}px ${T.fontUi}`;
    ctx.textBaseline = 'middle';
    for (const db of dbs) {
      const y = Math.round(dbScale.toY(db, rect.y, rect.h)) + 0.5;
      ctx.strokeStyle = opts.line || T.hairline;
      ctx.beginPath();
      ctx.moveTo(rect.x, y);
      ctx.lineTo(rect.x + rect.w, y);
      ctx.stroke();
      if (opts.labels !== false) {
        ctx.fillStyle = opts.labelColor || T.textMute;
        ctx.textAlign = 'right';
        ctx.fillText(String(db), rect.x - (opts.labelPad || 3), y);
      }
    }
    ctx.restore();
  }

  /* ── Small line/label legend (top-left row or explicit x/y) ────────────
   * items: [{ color, label, dashed }]. Returns the x cursor after the row. */
  function legend(ctx, items, x, y, opts) {
    opts = opts || {};
    ctx.save();
    ctx.font = `${opts.size || 8}px ${T.fontUi}`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    let lx = x;
    for (const it of items) {
      ctx.strokeStyle = it.color;
      ctx.lineWidth = 2;
      ctx.setLineDash(it.dashed ? [4, 2] : []);
      ctx.beginPath(); ctx.moveTo(lx, y); ctx.lineTo(lx + 14, y); ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = it.color;
      ctx.fillText(it.label, lx + 18, y);
      lx += 18 + ctx.measureText(it.label).width + 12;
    }
    ctx.restore();
    return lx;
  }

  /* ── StaticLayer: offscreen cache for the parts that don't change ──────
   * Grids/axes/labels are identical every frame — draw them once into an
   * offscreen canvas keyed by a signature string, then blit. Rebuilds only when
   * the signature (size, dpr, mode…) changes. */
  class StaticLayer {
    constructor() { this.c = document.createElement('canvas'); this.sig = null; }
    // draw(g, W, H) fills the offscreen in logical coords; blit copies it back.
    ensure(W, H, sig, draw) {
      const dpr = Math.max(1, window.devicePixelRatio || 1);
      const bw = Math.round(W * dpr), bh = Math.round(H * dpr);
      const key = sig + '|' + bw + 'x' + bh;
      if (this.sig === key) return this;
      this.c.width = bw; this.c.height = bh;
      const g = this.c.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      draw(g, W, H);
      this.sig = key;
      this._W = W; this._H = H;
      return this;
    }
    blit(ctx) { ctx.drawImage(this.c, 0, 0, this._W, this._H); }
  }

  AX.canvas = {
    fit, clear, makeFreqScale, makeDbScale, freqGrid, dbGrid, legend, StaticLayer,
    // Default full-range audio frequency scale (20 Hz – 20 kHz).
    freq: makeFreqScale(20, 20000),
  };

  /* ── AX.dsp — exact, C++-matched magnitude-response math ───────────────*/
  const AXdsp = {
    // Biquad |H(f)|² → dB. Coeffs are the normalized (a0=1) transfer function.
    biquadResponse(f, sr, b0, b1, b2, a1, a2) {
      const w = 2 * Math.PI * f / sr;
      const cw = Math.cos(w), sw = Math.sin(w);
      const c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
      const nR = b0 + b1 * cw + b2 * c2, nI = b1 * sw + b2 * s2;
      const dR = 1 + a1 * cw + a2 * c2, dI = a1 * sw + a2 * s2;
      const mag2 = (nR * nR + nI * nI) / (dR * dR + dI * dI);
      return 10 * Math.log10(Math.max(mag2, 1e-20));
    },
    // 1st-order section |H(f)|² → dB (b2=a2=0). Matches the saturator filter viz.
    firstOrderDb(f, sr, b0, b1, a1) {
      const w = 2 * Math.PI * f / sr;
      const cw = Math.cos(w), sw = Math.sin(w);
      const nR = b0 + b1 * cw, nI = b1 * sw;
      const dR = 1 + a1 * cw, dI = a1 * sw;
      return 10 * Math.log10(Math.max((nR * nR + nI * nI) / (dR * dR + dI * dI), 1e-20));
    },
    // RBJ cookbook coeffs for peaking / low_shelf / high_shelf, normalized by a0.
    rbjCoeffs(kind, fc, q, gainDb, sr) {
      const A = Math.pow(10, gainDb / 40);
      const w0 = 2 * Math.PI * fc / sr;
      const cw = Math.cos(w0), sw = Math.sin(w0);
      const alpha = sw / (2 * q);
      let b0, b1, b2, a0, a1, a2;
      if (kind === 'peaking') {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      } else if (kind === 'low_shelf') {
        const sq = 2 * Math.sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - sq;
      } else { // high_shelf
        const sq = 2 * Math.sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sq;
      }
      return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
    },
  };
  AX.dsp = AXdsp;
})();
