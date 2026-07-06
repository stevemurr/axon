"""HTML report over the axon-run/1 envelopes in artifacts/.

The JSON envelopes stay the machine-readable source of truth (result.json per
run); this module renders a single self-contained, dependency-free HTML view
of them — status tiles per tool + full run history — at
artifacts/report/index.html. Regenerated automatically after every run
(Run.finish) and on demand via `uv run axon report`.
"""
from __future__ import annotations

import datetime
import json
from pathlib import Path

TOOL_ORDER = ["test", "bench", "coverage", "eval-null", "eval-ssl-comp"]
TOOL_LABEL = {
    "test": "Tests",
    "bench": "Benchmark",
    "coverage": "Coverage",
    "eval-null": "Eval · Null",
    "eval-ssl-comp": "Eval · SSL Comp",
}


def collect(repo: Path) -> dict:
    """Scan artifacts/<tool>/<stamp>/result.json into {tool: [runs newest-first]}."""
    root = repo / "artifacts"
    tools: dict[str, list] = {}
    if not root.is_dir():
        return tools
    for tool_dir in sorted(root.iterdir()):
        if not tool_dir.is_dir() or tool_dir.name == "report":
            continue
        runs = []
        for run_dir in tool_dir.iterdir():
            if run_dir.name == "latest" or not run_dir.is_dir():
                continue
            rj = run_dir / "result.json"
            if not rj.is_file():
                continue
            try:
                env = json.loads(rj.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            env["stamp"] = run_dir.name
            env["path"] = f"../{tool_dir.name}/{run_dir.name}/"
            runs.append(env)
        if runs:
            runs.sort(key=lambda e: e["stamp"], reverse=True)
            tools[tool_dir.name] = runs
    return tools


def generate(repo: Path) -> Path:
    tools = collect(repo)
    meta = {
        "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "tool_order": [t for t in TOOL_ORDER if t in tools]
                      + [t for t in sorted(tools) if t not in TOOL_ORDER],
        "tool_label": TOOL_LABEL,
    }
    payload = json.dumps({"meta": meta, "tools": tools}).replace("</", "<\\/")
    out = repo / "artifacts" / "report" / "index.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(_TEMPLATE.replace("__DATA__", payload))
    return out


_TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Axon — run report</title>
<style>
  :root {
    --surface: #fcfcfb; --surface-2: #f4f4f2; --border: #e4e3df;
    --ink: #0b0b0b; --ink-2: #52514e; --ink-3: #8a887f;
    --good: #0ca30c; --warn: #fab219; --crit: #d03b3b; --accent: #2a78d6;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --surface: #1a1a19; --surface-2: #232322; --border: #3a3936;
      --ink: #ffffff; --ink-2: #c3c2b7; --ink-3: #8a887f;
      --accent: #3987e5;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--surface); color: var(--ink);
    font: 14px/1.5 -apple-system, "SF Pro Text", "Segoe UI", system-ui, sans-serif;
  }
  main { max-width: 1080px; margin: 0 auto; padding: 32px 24px 64px; }
  header { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; margin-bottom: 24px; }
  header h1 { font-size: 20px; font-weight: 650; margin: 0; letter-spacing: -0.01em; }
  header .sub { color: var(--ink-3); font-size: 13px; }
  code, .mono { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 12.5px; }

  .tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 12px; margin-bottom: 36px; }
  .tile {
    background: var(--surface-2); border: 1px solid var(--border); border-radius: 10px;
    padding: 14px 16px 12px; display: flex; flex-direction: column; gap: 4px; min-height: 118px;
  }
  .tile .tool { font-size: 12px; font-weight: 600; letter-spacing: 0.04em; text-transform: uppercase; color: var(--ink-3); }
  .tile .status { display: flex; align-items: center; gap: 7px; font-size: 19px; font-weight: 700; }
  .dot { width: 9px; height: 9px; border-radius: 50%; flex: none; }
  .pass .dot { background: var(--good); } .fail .dot { background: var(--crit); }
  .pass .word { color: var(--good); } .fail .word { color: var(--crit); }
  .tile .headline { font-size: 13px; color: var(--ink); }
  .tile .when { font-size: 12px; color: var(--ink-3); margin-top: auto; }
  .tile svg.spark { display: block; margin-top: 2px; }
  .spark polyline { fill: none; stroke: var(--accent); stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }

  section { margin-bottom: 32px; }
  section h2 { font-size: 15px; font-weight: 650; margin: 0 0 8px; }
  table { width: 100%; border-collapse: collapse; }
  th { text-align: left; font-size: 11.5px; font-weight: 600; letter-spacing: 0.05em;
       text-transform: uppercase; color: var(--ink-3); padding: 6px 10px; border-bottom: 1px solid var(--border); }
  td { padding: 7px 10px; border-bottom: 1px solid var(--border); vertical-align: top; }
  tr:hover td { background: var(--surface-2); }
  td.status-cell { white-space: nowrap; font-weight: 600; }
  td.status-cell .dot { display: inline-block; margin-right: 6px; }
  td.when-cell, td.dur, td.git { color: var(--ink-2); white-space: nowrap; }
  td .flake { color: var(--warn); font-weight: 600; }
  a { color: var(--accent); text-decoration: none; }
  a:hover { text-decoration: underline; }
  .empty { color: var(--ink-3); padding: 24px 0; }
  .dirty { color: var(--warn); }

  .tile { cursor: pointer; }
  .tile:hover { border-color: var(--ink-3); }
  tr.run-row { cursor: pointer; }
  td.chev-cell { width: 20px; color: var(--ink-3); }
  .chev { display: inline-block; transition: transform 0.12s; }
  tr[aria-expanded="true"] .chev { transform: rotate(90deg); }
  tr.detail-row > td {
    background: var(--surface-2); padding: 14px 18px 16px;
    border-bottom: 1px solid var(--border);
  }
  tr.detail-row:hover > td { background: var(--surface-2); }
  .kv { display: grid; grid-template-columns: max-content 1fr; gap: 3px 18px; margin: 0 0 10px; }
  .kv .k { font-size: 11.5px; font-weight: 600; letter-spacing: 0.05em;
           text-transform: uppercase; color: var(--ink-3); padding-top: 1px; }
  .kv .v { font-size: 13px; }
  .detail-h { font-size: 11.5px; font-weight: 600; letter-spacing: 0.05em;
              text-transform: uppercase; color: var(--ink-3); margin: 10px 0 4px; }
  .arts { display: flex; flex-wrap: wrap; gap: 6px 14px; }
</style>
</head>
<body>
<script id="data" type="application/json">__DATA__</script>
<main>
  <header>
    <h1>Axon — run report</h1>
    <span class="sub" id="gen"></span>
  </header>
  <div class="tiles" id="tiles"></div>
  <div id="sections"></div>
</main>
<script>
const DATA = JSON.parse(document.getElementById('data').textContent);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};
const label = t => (DATA.meta.tool_label || {})[t] || t;
const parseStamp = s => {
  const m = s.match(/^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})$/);
  return m ? new Date(+m[1], +m[2]-1, +m[3], +m[4], +m[5], +m[6]) : null;
};
const ago = s => {
  const d = parseStamp(s); if (!d) return s;
  const mins = Math.round((Date.now() - d.getTime()) / 60000);
  if (mins < 1) return 'just now';
  if (mins < 60) return mins + ' min ago';
  const h = Math.round(mins / 60);
  if (h < 48) return h + ' h ago';
  return Math.round(h / 24) + ' d ago';
};
const fmtWhen = s => { const d = parseStamp(s); return d ? d.toLocaleString() : s; };

document.getElementById('gen').textContent = 'generated ' + DATA.meta.generated;

const headlineFor = (tool, run) => {
  const m = run.metrics || {};
  if (tool === 'test' && m.tests != null) return (m.tests - (m.failures||0)) + '/' + m.tests + ' passed';
  if (tool === 'coverage' && m.line_pct != null) return m.line_pct + '% lines';
  if (tool === 'bench' && m.cells != null) return m.cells + ' cells · ' + (m.deadline_misses||0) + ' misses';
  if (tool === 'eval-null' && m.sets != null)
    return (m.sets - (m.failed||0)) + '/' + m.sets + ' null' + (m.flakes ? ' · ' + m.flakes + ' flake' : '');
  return run.summary || '';
};

// -------- tiles
const tiles = document.getElementById('tiles');
for (const tool of DATA.meta.tool_order) {
  const runs = DATA.tools[tool]; const latest = runs[0];
  const ok = latest.status === 'pass';
  const tile = el('div', 'tile ' + (ok ? 'pass' : 'fail'));
  tile.appendChild(el('div', 'tool', label(tool)));
  const st = el('div', 'status');
  st.appendChild(el('span', 'dot'));
  st.appendChild(el('span', 'word', ok ? 'PASS' : 'FAIL'));
  tile.appendChild(st);
  tile.appendChild(el('div', 'headline', headlineFor(tool, latest)));
  // Coverage trend sparkline (oldest -> newest), decorative: table below has values.
  if (tool === 'coverage') {
    const pts = runs.map(r => (r.metrics||{}).line_pct).filter(v => v != null).reverse();
    if (pts.length >= 2) {
      const w = 150, h = 26, lo = Math.min(...pts), hi = Math.max(...pts), span = (hi-lo) || 1;
      const xy = pts.map((v,i) => (i*(w-4)/(pts.length-1)+2) + ',' +
                                  (h-3-((v-lo)/span)*(h-8)));
      const svg = document.createElementNS('http://www.w3.org/2000/svg','svg');
      svg.setAttribute('class','spark'); svg.setAttribute('width',w); svg.setAttribute('height',h);
      svg.setAttribute('role','img');
      const t = document.createElementNS('http://www.w3.org/2000/svg','title');
      t.textContent = 'line coverage: ' + pts.join('% → ') + '%';
      const pl = document.createElementNS('http://www.w3.org/2000/svg','polyline');
      pl.setAttribute('points', xy.join(' '));
      svg.appendChild(t); svg.appendChild(pl); tile.appendChild(svg);
    }
  }
  tile.appendChild(el('div', 'when', ago(latest.stamp) + ' · ' + latest.duration_s + ' s'));
  tile.title = 'jump to ' + label(tool) + ' history';
  tile.tabIndex = 0;
  const jump = () => document.getElementById('sec-' + tool)
                             .scrollIntoView({behavior: 'smooth', block: 'start'});
  tile.addEventListener('click', jump);
  tile.addEventListener('keydown', e => { if (e.key === 'Enter') jump(); });
  tiles.appendChild(tile);
}

// -------- detail panel (expandable row; all data is already embedded)
const kvBlock = (title, obj) => {
  const frag = document.createDocumentFragment();
  const keys = Object.keys(obj || {});
  if (!keys.length) return frag;
  frag.appendChild(el('div', 'detail-h', title));
  const kv = el('div', 'kv');
  for (const k of keys) {
    kv.appendChild(el('div', 'k', k));
    const v = obj[k];
    kv.appendChild(el('div', 'v mono',
      Array.isArray(v) ? v.join('  →  ') : String(v)));
  }
  frag.appendChild(kv);
  return frag;
};

const buildDetail = (tool, run) => {
  const td = el('td');
  td.colSpan = 7;
  td.appendChild(kvBlock('Run', {
    stamp: run.stamp,
    status: run.status,
    summary: run.summary || '',
    duration: run.duration_s + ' s',
    git: ((run.git||{}).rev || '?') + ((run.git||{}).dirty ? ' (+dirty)' : ''),
  }));
  td.appendChild(kvBlock('Metrics', run.metrics));
  td.appendChild(kvBlock('Details', run.details));
  const arts = ['result.json', ...(run.artifacts || [])];
  td.appendChild(el('div', 'detail-h', 'Artifacts'));
  const row = el('div', 'arts');
  for (const name of arts) {
    const a = el('a', 'mono', name);
    a.href = run.path + name;
    row.appendChild(a);
  }
  td.appendChild(row);
  const tr = el('tr', 'detail-row');
  tr.appendChild(td);
  return tr;
};

// -------- history sections
const sections = document.getElementById('sections');
for (const tool of DATA.meta.tool_order) {
  const sec = el('section');
  sec.id = 'sec-' + tool;
  sec.appendChild(el('h2', null, label(tool)));
  const tbl = el('table');
  const hr = el('tr');
  for (const h of ['', 'When', 'Status', 'Summary', 'Duration', 'Rev', 'Run'])
    hr.appendChild(el('th', null, h));
  tbl.appendChild(hr);
  for (const run of DATA.tools[tool]) {
    const ok = run.status === 'pass';
    const tr = el('tr', 'run-row');
    tr.tabIndex = 0;
    tr.setAttribute('aria-expanded', 'false');
    const chevTd = el('td', 'chev-cell');
    chevTd.appendChild(el('span', 'chev', '▸'));
    tr.appendChild(chevTd);
    tr.appendChild(el('td', 'when-cell', fmtWhen(run.stamp)));
    const st = el('td', 'status-cell ' + (ok ? 'pass' : 'fail'));
    st.appendChild(el('span', 'dot'));
    st.appendChild(el('span', 'word', ok ? 'PASS' : 'FAIL'));
    tr.appendChild(st);
    const sum = el('td');
    sum.appendChild(document.createTextNode(run.summary || ''));
    if ((run.metrics||{}).flakes) sum.appendChild(el('span', 'flake', '  ⚠ flake'));
    tr.appendChild(sum);
    tr.appendChild(el('td', 'dur', run.duration_s + ' s'));
    const g = el('td', 'git mono', (run.git||{}).rev || '');
    if ((run.git||{}).dirty) { g.appendChild(el('span', 'dirty', ' +dirty')); }
    tr.appendChild(g);
    const link = el('td');
    const a = el('a', 'mono', run.stamp + '/');
    a.href = run.path;
    a.addEventListener('click', e => e.stopPropagation());  // link, not toggle
    link.appendChild(a);
    tr.appendChild(link);
    tbl.appendChild(tr);

    let detail = null;  // built lazily on first expand
    const toggle = () => {
      const open = tr.getAttribute('aria-expanded') === 'true';
      if (open) {
        detail.remove();
        tr.setAttribute('aria-expanded', 'false');
      } else {
        if (!detail) detail = buildDetail(tool, run);
        tr.after(detail);
        tr.setAttribute('aria-expanded', 'true');
      }
    };
    tr.addEventListener('click', toggle);
    tr.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); }
    });
  }
  sec.appendChild(tbl);
  sections.appendChild(sec);
}
if (!DATA.meta.tool_order.length)
  sections.appendChild(el('div', 'empty', 'No runs yet — run `uv run axon test|bench|coverage|eval …` first.'));
</script>
</body>
</html>
"""
