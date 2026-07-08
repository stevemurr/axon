/* ============================================================================
 * Axon Design System — Component Kit (AX.Knob / Toggle / EnumSelect / Button / Card)
 * ----------------------------------------------------------------------------
 * Declarative widget factories that replace the old buildControl()/buildKnobSVG
 * tangle. Each returns { el, update(value) } (plus a few widget-specific hooks)
 * so the registry can build a control surface from data and route host pushes
 * back to the right widget. Interaction (drag, wheel, double-click-to-default,
 * keyboard) and hover/focus/active states are defined ONCE here, from tokens.
 *
 * Contract for value widgets:
 *   opts.meta    { min, max, def, name }   — parameter descriptor
 *   opts.value   initial value
 *   opts.format  (v) => string             — display formatter
 *   opts.label   display label (defaults to meta.name)
 *   opts.accent  arc / active colour (defaults to --accent)
 *   opts.onInput (v) => void               — user changed it (send + reflect)
 *   .update(v)   set displayed value programmatically (host push; no onInput)
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = (window.AX = window.AX || {});
  const T = AX.tokens;

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const el = (tag, cls, html) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html != null) e.innerHTML = html;
    return e;
  };

  /* ── Knob geometry (SVG arc) ───────────────────────────────────────────*/
  const K = { SIZE: 56, CX: 28, CY: 28, R: 22, CAP: 12.5, A0: -135, A1: 135 };
  const d2r = (d) => (d - 90) * Math.PI / 180;
  const polar = (cx, cy, r, a) => [cx + r * Math.cos(d2r(a)), cy + r * Math.sin(d2r(a))];
  function arcPath(cx, cy, r, a0, a1) {
    const [sx, sy] = polar(cx, cy, r, a0);
    const [ex, ey] = polar(cx, cy, r, a1);
    const large = (a1 - a0 + 360) % 360 > 180 ? 1 : 0;
    return `M ${sx.toFixed(2)} ${sy.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${ex.toFixed(2)} ${ey.toFixed(2)}`;
  }
  // A shared cap gradient id is safe: every knob's cap is identical, so browsers
  // resolving url(#axKnobCap) to the first definition is exactly what we want.
  function knobSvg(norm, accent) {
    const ang = K.A0 + clamp(norm, 0, 1) * (K.A1 - K.A0);
    const track = arcPath(K.CX, K.CY, K.R, K.A0, K.A1);
    const fill = Math.abs(ang - K.A0) < 0.5 ? '' : arcPath(K.CX, K.CY, K.R, K.A0, ang);
    const [ox, oy] = polar(K.CX, K.CY, K.CAP - 1.5, ang);   // pointer tip (cap edge)
    const [ix, iy] = polar(K.CX, K.CY, 4.5, ang);           // pointer root (near centre)
    const [dx, dy] = polar(K.CX, K.CY, K.R, ang);           // value marker on the arc
    return `<svg width="${K.SIZE}" height="${K.SIZE}" viewBox="0 0 ${K.SIZE} ${K.SIZE}" shape-rendering="geometricPrecision" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <radialGradient id="axKnobCap" cx="50%" cy="34%" r="72%">
          <stop offset="0%" stop-color="${T.surfaceHover}"/>
          <stop offset="100%" stop-color="${T.bgSunken}"/>
        </radialGradient>
      </defs>
      <path d="${track}" fill="none" stroke="${T.border}" stroke-width="4" stroke-linecap="round"/>
      ${fill ? `<path d="${fill}" fill="none" stroke="${accent}" stroke-width="4" stroke-linecap="round"/>` : ''}
      ${fill ? `<circle cx="${dx.toFixed(2)}" cy="${dy.toFixed(2)}" r="2.6" fill="${accent}"/>` : ''}
      <circle cx="${K.CX}" cy="${K.CY}" r="${K.CAP}" fill="url(#axKnobCap)" stroke="${T.borderStrong}" stroke-width="1"/>
      <path d="M ${(K.CX - 7).toFixed(2)} ${(K.CY - 8).toFixed(2)} A ${K.CAP - 1} ${K.CAP - 1} 0 0 1 ${(K.CX + 7).toFixed(2)} ${(K.CY - 8).toFixed(2)}" fill="none" stroke="${T.hairlineStrong}" stroke-width="1" stroke-linecap="round"/>
      <line x1="${ix.toFixed(2)}" y1="${iy.toFixed(2)}" x2="${ox.toFixed(2)}" y2="${oy.toFixed(2)}" stroke="${accent}" stroke-width="2.5" stroke-linecap="round"/>
    </svg>`;
  }

  /* ── Knob ──────────────────────────────────────────────────────────────*/
  function Knob(opts) {
    const m = opts.meta, accent = opts.accent || T.accent;
    const fmt = opts.format || ((v) => v.toFixed(2));
    let value = opts.value != null ? opts.value : m.def;
    const norm = () => (m.max === m.min ? 0 : clamp((value - m.min) / (m.max - m.min), 0, 1));

    const wrap = el('div', 'ax-knob');
    wrap.tabIndex = 0;
    const slot = el('div', 'ax-knob__svg');
    const label = el('div', 'ax-knob__label');
    const val = el('div', 'ax-knob__val');
    label.textContent = opts.label || m.name;
    wrap.append(slot, label, val);

    function paint() { slot.innerHTML = knobSvg(norm(), accent); val.textContent = fmt(value); }
    function commit(v) { value = clamp(v, m.min, m.max); paint(); opts.onInput && opts.onInput(value); }
    paint();

    // Vertical drag (pointer) — 200px of travel spans the full range.
    let startY = 0, startV = 0, dragging = false;
    wrap.addEventListener('pointerdown', (e) => {
      dragging = true; startY = e.clientY; startV = value;
      wrap.setPointerCapture(e.pointerId);
      wrap.classList.add('is-active');
      e.preventDefault();
    });
    wrap.addEventListener('pointermove', (e) => {
      if (!dragging) return;
      const span = (m.max - m.min);
      const fine = e.shiftKey ? 0.25 : 1;             // Shift = fine adjust
      commit(startV + (startY - e.clientY) / 200 * span * fine);
    });
    const end = (e) => { if (dragging) { dragging = false; wrap.classList.remove('is-active'); if (e && e.pointerId != null) try { wrap.releasePointerCapture(e.pointerId); } catch (_) {} } };
    wrap.addEventListener('pointerup', end);
    wrap.addEventListener('pointercancel', end);
    wrap.addEventListener('dblclick', () => commit(m.def));
    wrap.addEventListener('wheel', (e) => {
      e.preventDefault();
      const span = (m.max - m.min);
      commit(value - Math.sign(e.deltaY) * span * (e.shiftKey ? 0.005 : 0.02));
    }, { passive: false });
    wrap.addEventListener('keydown', (e) => {
      const span = (m.max - m.min), step = span * (e.shiftKey ? 0.005 : 0.02);
      if (e.key === 'ArrowUp' || e.key === 'ArrowRight') { commit(value + step); e.preventDefault(); }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') { commit(value - step); e.preventDefault(); }
      else if (e.key === 'Home') { commit(m.def); e.preventDefault(); }
    });

    return {
      el: wrap,
      update(v) { value = clamp(v, m.min, m.max); paint(); },
      relabel(text) { label.textContent = text; },
    };
  }

  /* ── Toggle (on/off with id-specific labels) ───────────────────────────*/
  function Toggle(opts) {
    const m = opts.meta, accent = opts.accent || T.accent;
    const [offL, onL] = opts.labels || ['OFF', 'ON'];
    let on = (opts.value != null ? opts.value : m.def) >= 0.5;

    const wrap = el('div', 'ax-toggle');
    const btn = el('button', 'ax-toggle__btn');
    const label = el('div', 'ax-toggle__label');
    label.textContent = opts.label || m.name;
    btn.style.setProperty('--w-accent', accent);
    wrap.append(btn, label);

    function paint() { btn.textContent = on ? onL : offL; btn.classList.toggle('is-on', on); }
    paint();
    btn.addEventListener('click', () => { on = !on; paint(); opts.onInput && opts.onInput(on ? 1 : 0); });

    return { el: wrap, update(v) { on = v >= 0.5; paint(); } };
  }

  /* ── EnumSelect (styled native <select>) ───────────────────────────────*/
  function EnumSelect(opts) {
    const m = opts.meta;
    const options = m.enumOptions || [];
    let idx = clamp(Math.round(opts.value != null ? opts.value : m.def), 0, options.length - 1);

    const wrap = el('div', 'ax-enum');
    const sel = el('select', 'ax-enum__select');
    options.forEach((o, i) => {
      const op = el('option'); op.value = String(i);
      op.textContent = opts.pretty ? opts.pretty(o) : o;
      if (i === idx) op.selected = true;
      sel.appendChild(op);
    });
    const label = el('div', 'ax-enum__label');
    label.textContent = opts.label || m.name;
    wrap.append(sel, label);
    sel.addEventListener('change', () => {
      const v = parseInt(sel.value, 10);
      if (Number.isFinite(v)) { idx = v; opts.onInput && opts.onInput(v); }
    });

    return { el: wrap, update(v) { idx = clamp(Math.round(v), 0, options.length - 1); if (sel.selectedIndex !== idx) sel.selectedIndex = idx; } };
  }

  /* ── Button (momentary action; e.g. CALIBRATE / RESET) ─────────────────*/
  function Button(opts) {
    const wrap = el('div', 'ax-toggle');
    const btn = el('button', 'ax-toggle__btn' + (opts.variant === 'danger' ? ' is-danger' : ''));
    btn.textContent = opts.text || 'GO';
    const label = el('div', 'ax-toggle__label');
    label.textContent = opts.label || '';
    wrap.append(btn, label);
    btn.addEventListener('click', () => opts.onClick && opts.onClick());
    return { el: wrap, flash() { btn.classList.add('is-on'); setTimeout(() => btn.classList.remove('is-on'), 140); } };
  }

  /* ── Card (chain-strip processor tab) ──────────────────────────────────*/
  function Card(opts) {
    const wrap = el('div', 'ax-card');
    wrap.style.setProperty('--c-accent', opts.accent || T.accent);
    const name = el('div', 'ax-card__name');
    const dot = el('div', 'ax-card__dot');
    name.textContent = opts.name;
    wrap.append(name, dot);
    return {
      el: wrap,
      setActive(b) { dot.classList.toggle('is-active', !!b); },
      setSelected(b) { wrap.classList.toggle('is-selected', !!b); },
    };
  }

  AX.Knob = Knob;
  AX.Toggle = Toggle;
  AX.EnumSelect = EnumSelect;
  AX.Button = Button;
  AX.Card = Card;
  AX.knobSvg = knobSvg; // exported for the gallery
})();
