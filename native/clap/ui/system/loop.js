/* ============================================================================
 * Axon Design System — Render Loop (AX.loop)
 * ----------------------------------------------------------------------------
 * ONE requestAnimationFrame loop owns every repaint. Native pushes
 * (axonMeters / axonSpectrum / axonLimiter / …) only update target state and
 * call AX.loop.requestPaint(); the loop then eases displayed values toward those
 * targets at display refresh (~60fps) — decoupled from the ~21fps audio cadence,
 * so meters and curves glide instead of stepping.
 *
 * Painter contract: a draw fn returns TRUE while it is still animating (values
 * not yet settled). The loop keeps ticking while any painter is animating or a
 * new push arrived, then fully stops (zero idle CPU) until the next push or
 * interaction. It pauses entirely when the GUI is hidden.
 * ==========================================================================*/
(function () {
  'use strict';
  const AX = (window.AX = window.AX || {});

  // Per-frame easing toward a target. coef in (0,1]; higher = snappier.
  // Frame-rate independent-ish for the ~60fps we run at.
  AX.ease = (cur, tgt, coef) => cur + (tgt - cur) * coef;
  AX.settled = (a, b, eps) => Math.abs(a - b) <= (eps || 1e-3);

  const always = [];     // always-on painters: () => bool(animating)
  let moduleDraw = null;  // selected module's painter, or null
  let running = false, idle = true, dirty = true, wasMoving = false, rafId = null;

  function schedule() { if (rafId == null) rafId = requestAnimationFrame(tick); }

  function tick() {
    rafId = null;
    if (!running) { idle = true; return; }
    let moving = false;
    if (dirty || wasMoving) {
      for (let i = 0; i < always.length; i++) { if (always[i]()) moving = true; }
      if (moduleDraw && moduleDraw()) moving = true;
      dirty = false;
    }
    wasMoving = moving;
    if (moving) schedule();
    else idle = true; // settled — stop until the next push/interaction
  }

  AX.loop = {
    add(drawFn) { always.push(drawFn); return drawFn; },
    setModule(drawFn) { moduleDraw = drawFn || null; this.requestPaint(); },
    requestPaint() { dirty = true; if (running && idle) { idle = false; schedule(); } },
    start() { running = true; idle = false; dirty = true; schedule(); },
    stop() { running = false; if (rafId != null) { cancelAnimationFrame(rafId); rafId = null; } idle = true; },
    isRunning() { return running; },
  };

  // Pause when the host hides the plugin window. C++ can call axonVisible()
  // from gui_show/gui_hide; Page Visibility is the browser/dev fallback.
  window.axonVisible = function (v) { if (v) AX.loop.start(); else AX.loop.stop(); };
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) AX.loop.stop(); else AX.loop.start();
  });
})();
