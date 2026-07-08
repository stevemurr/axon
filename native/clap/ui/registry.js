/* ============================================================================
 * Axon Design System — Core State + Module Registry
 * ----------------------------------------------------------------------------
 * The single place that (a) holds live parameter state, (b) talks to the native
 * host over the (unchanged) bridge, (c) formats values, and (d) collects module
 * descriptors. A module is now DATA: one AX.registerModule({...}) call per stage
 * replaces the old scatter of PROCESSORS + STAGE_COLORS + STAGE_NAMES +
 * toggleLabels + knobLabel + renderSurface show-toggles.
 *
 * Descriptor shape (all but id/name/params optional):
 *   {
 *     id, name,                       // StageID + display name
 *     accent,                         // identity colour (default AX.stageAccent(id))
 *     params: ['SEQ_ON', ...],        // control ids, in display order (flat)
 *     groups: [{label?, params:[...]}],// optional channel-strip layout: a row of
 *                                       // vertical columns instead of a flat row
 *     wetParams: ['SEQ_ON'],          // ids whose >0 lights the "active" dot
 *     labels: { SEQ_LF_BELL:['SHELF','BELL'] },   // toggle off/on labels
 *     dynLabel(id, values) -> string, // optional live knob relabel (limiter)
 *     format:  { ID: (v)=>string },   // per-id display override
 *     rebuildOn: ['MLA'],             // param changes that rebuild the surface
 *     visualizer: {                   // optional live viz under the controls
 *        build(host),                 //   create canvas DOM
 *        draw() -> bool,              //   paint; return true while animating
 *     },
 *     telemetry: { axonLimiter(d){...} },  // native push endpoints this module owns
 *   }
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = (window.AX = window.AX || {});

  /* ── Live state ────────────────────────────────────────────────────────*/
  AX.state = { meta: {}, values: {}, order: [], selected: null };

  const stateVal = (id) => {
    const m = AX.state.meta[id];
    return AX.state.values[id] != null ? AX.state.values[id] : (m ? m.def : 0);
  };
  AX.val = stateVal;

  /* ── Bridge (unchanged contract) ───────────────────────────────────────*/
  function post(msg) {
    const w = window.webkit;
    if (w && w.messageHandlers && w.messageHandlers.axon) w.messageHandlers.axon.postMessage(msg);
  }
  AX.bridge = {
    sendParam(id, value) { AX.state.values[id] = value; post({ type: 'setParam', id, value }); },
    sendOrder(order) { post({ type: 'reorder', order: [...order] }); },
  };

  /* ── Value formatting ──────────────────────────────────────────────────*/
  function prettyEnum(s) {
    if (typeof s !== 'string') return String(s);
    return s.replace(/_/g, ' ').toUpperCase();
  }
  function format(id, val) {
    const m = AX.state.meta[id];
    if (!m) return val.toFixed(2);
    const unit = m.unit;
    if (unit === 'enum' && Array.isArray(m.enumOptions) && m.enumOptions.length) {
      const idx = Math.max(0, Math.min(m.enumOptions.length - 1, Math.round(val)));
      return prettyEnum(m.enumOptions[idx]);
    }
    if (unit === 'dB')   return `${val >= 0 ? '+' : ''}${val.toFixed(1)} dB`;
    if (unit === 'LUFS') return `${val.toFixed(1)} L`;
    if (unit === 'Hz')   return val >= 1000 ? `${(val / 1000).toFixed(val >= 10000 ? 1 : 2)} kHz` : `${Math.round(val)} Hz`;
    if (unit === 'ms')   return `${Math.round(val)} ms`;
    if (unit === 'switch') return val >= 0.5 ? 'LIMIT' : 'COMP';
    if (unit === '' && m.max <= 1) return `${Math.round(val * 100)}%`;
    return val.toFixed(2);
  }
  AX.format = format;
  AX.prettyEnum = prettyEnum;

  /* ── Module registry ───────────────────────────────────────────────────*/
  const modules = [];
  const byId = {};
  AX.registerModule = function (desc) {
    if (byId[desc.id]) { const i = modules.indexOf(byId[desc.id]); if (i >= 0) modules.splice(i, 1); }
    desc.accent = desc.accent || AX.stageAccent(desc.id);
    desc.wetParams = desc.wetParams || [];
    desc.labels = desc.labels || {};
    modules.push(desc);
    byId[desc.id] = desc;
    return desc;
  };
  AX.modules = {
    get: (id) => byId[id],
    list: () => modules.slice(),
    ids: () => modules.map((m) => m.id),
    // Is any wet/enable param for this module engaged?
    isActive(desc) {
      return (desc.wetParams || []).some((id) => stateVal(id) > 0);
    },
    // Off/on labels for a toggle id within a module.
    toggleLabels(desc, id) {
      return (desc.labels && desc.labels[id]) || ['OFF', 'ON'];
    },
    // Live knob label (limiter relabels Attack/Release in Dynamic mode).
    knobLabel(desc, id) {
      if (desc.dynLabel) { const l = desc.dynLabel(id, AX.state.values); if (l) return l; }
      const m = AX.state.meta[id];
      return m ? m.name : id;
    },
    // Per-id display formatter (falls back to global format()).
    format(desc, id, v) {
      if (desc.format && desc.format[id]) return desc.format[id](v);
      return format(id, v);
    },
  };
})();
