#!/usr/bin/env python3
# Pack a --route-trace session into one self-contained HTML viewer.
#
# Companion to route-analyze.py: that one answers questions in a terminal, this one puts every
# routed expert on a page you can scroll — the step x layer matrix with the expert ids in its
# cells, hot experts per layer, the per-token metrics, and the raw rows. Stdlib only, and the
# data is packed into the page, so the result opens anywhere with no network and no spreadsheet.
#
#     python scripts/route-viewer.py docs/bench-data/<session>/ viewer.html
#
# Reads every <tag>.route.csv in the session directory that has a <tag>.metrics sink beside it.
import json, os, sys
from collections import defaultdict, Counter

D = sys.argv[1]
OUT = sys.argv[2]
MIB = 1048576.0


def discover(d):
    """Every traced model in the session: <tag>.route.csv with a <tag>.metrics beside it."""
    out = []
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".route.csv"):
            continue
        tag = fn[: -len(".route.csv")]
        if not os.path.exists(os.path.join(d, tag + ".metrics")):
            print("skipping %s: no %s.metrics beside it" % (tag, tag))
            continue
        out.append(tag)
    return out


def load_route(path):
    meta, eb, db = {}, {}, {}
    cells = defaultdict(list)          # (phase, step, layer) -> [(slot,e,w,r,bytes)]
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                kv = dict(t.split("=", 1) for t in line[1:].split() if "=" in t)
                if "layer" in kv and "expert_bytes" in kv:
                    eb[int(kv["layer"])] = int(kv["expert_bytes"])
                    db[int(kv["layer"])] = int(kv["dense_bytes"])
                elif "n_expert" in kv:
                    meta = kv
                continue
            if line.startswith("turn"):
                continue
            p = line.rstrip("\n").split(",")
            if len(p) < 9:
                continue
            ph, st, l, sl, e = int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])
            w = 0.0 if p[6] == "nan" else float(p[6])
            cells[(ph, st, l)].append((sl, e, w, int(p[7]), int(p[8])))
    return meta, eb, db, cells


def load_metrics(path):
    rows = []
    summary = {}
    with open(path) as f:
        for line in f:
            if line.startswith("# summary"):
                for tok in line.split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        summary[k] = v
                continue
            rows.append(line.rstrip("\n"))
    hdr = rows[0].split(",")
    out = []
    for r in rows[1:]:
        if not r.strip():
            continue
        out.append(r.split(","))
    return hdr, out, summary


payload = {}
for tag in discover(D):
    meta, eb, db, cells = load_route(f"{D}/{tag}.route.csv")
    hdr, mrows, summary = load_metrics(f"{D}/{tag}.metrics")
    # The preamble records the model path the run used; its basename is the friendliest label.
    label = os.path.basename(meta.get("model", "")).replace(".gguf", "") or tag
    k = int(meta["n_expert_used"])
    ne = int(meta["n_expert"])
    moe = sorted([l for l in eb if eb[l] > 0])

    # compact cell encoding: "phase|step|layer|e:w4:r;e:w4:r;..."
    lines = []
    for (ph, st, l) in sorted(cells):
        rec = sorted(cells[(ph, st, l)])
        body = ";".join("%d:%d:%d" % (e, round(w * 10000), r) for _s, e, w, r, _b in rec)
        lines.append("%d|%d|%d|%s" % (ph, st, l, body))

    # derived stats (decode only)
    uniq = defaultdict(set)
    freq = defaultdict(Counter)
    for (ph, st, l), rec in cells.items():
        if ph != 1:
            continue
        for _s, e, _w, _r, _b in rec:
            uniq[l].add(e)
            freq[l][e] += 1
    per_expert = sum(eb[l] for l in moe) / len(moe) / MIB
    bank = sum(eb[l] * ne for l in moe) / MIB
    touched = sum(eb[l] * len(uniq[l]) for l in moe) / MIB
    dense = sum(db.values()) / MIB

    # residency split (decode)
    res = [0, 0, 0]
    for (ph, st, l), rec in cells.items():
        if ph != 1:
            continue
        for _s, _e, _w, r, _b in rec:
            res[r] += 1

    payload[tag] = {
        "label": label, "arch": meta["arch"], "model": meta.get("model", ""),
        "n_layer": int(meta["n_layer"]), "n_expert": ne, "k": k, "moe": moe,
        "expert_bytes": {str(l): eb[l] for l in moe},
        "dense_bytes": {str(l): db[l] for l in sorted(db)},
        "per_expert_mib": round(per_expert, 2), "bank_mib": round(bank),
        "touched_mib": round(touched), "dense_mib": round(dense),
        "res": res,
        "summary": summary,
        "metrics_hdr": hdr, "metrics": mrows,
        "hot": {str(l): freq[l].most_common(12) for l in moe},
        "uniq": {str(l): len(uniq[l]) for l in moe},
        "cells": "\n".join(lines),
    }

DATA_JSON = json.dumps(payload, separators=(",", ":"))

HTML = r"""<title>Route trace — MoE expert routing, on device</title>
<style>
:root{
  --ground:#F5F8F8; --surface:#FFFFFF; --sunk:#EDF2F2; --line:#DAE3E4; --line-soft:#E8EEEE;
  --ink:#0F1A1C; --ink-2:#3B4C50; --muted:#67797E;
  --accent:#0E8C80; --accent-soft:#D5EDEA;
  --hot:#B65A12; --hot-soft:#F7E3D2;
  --shadow:0 1px 2px rgba(15,26,28,.06),0 8px 24px -12px rgba(15,26,28,.18);
}
@media (prefers-color-scheme:dark){
  :root{
    --ground:#0C1113; --surface:#141C1E; --sunk:#101719; --line:#253236; --line-soft:#1C282B;
    --ink:#E2ECEE; --ink-2:#A8BCC0; --muted:#7B9096;
    --accent:#3FC0B0; --accent-soft:#12312F;
    --hot:#E08A43; --hot-soft:#33210F;
    --shadow:0 1px 2px rgba(0,0,0,.4),0 12px 32px -14px rgba(0,0,0,.7);
  }
}
:root[data-theme="dark"]{
  --ground:#0C1113; --surface:#141C1E; --sunk:#101719; --line:#253236; --line-soft:#1C282B;
  --ink:#E2ECEE; --ink-2:#A8BCC0; --muted:#7B9096;
  --accent:#3FC0B0; --accent-soft:#12312F;
  --hot:#E08A43; --hot-soft:#33210F;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 12px 32px -14px rgba(0,0,0,.7);
}
:root[data-theme="light"]{
  --ground:#F5F8F8; --surface:#FFFFFF; --sunk:#EDF2F2; --line:#DAE3E4; --line-soft:#E8EEEE;
  --ink:#0F1A1C; --ink-2:#3B4C50; --muted:#67797E;
  --accent:#0E8C80; --accent-soft:#D5EDEA;
  --hot:#B65A12; --hot-soft:#F7E3D2;
  --shadow:0 1px 2px rgba(15,26,28,.06),0 8px 24px -12px rgba(15,26,28,.18);
}
*{box-sizing:border-box}
body{
  margin:0;background:var(--ground);color:var(--ink);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  font-size:15px;line-height:1.55;-webkit-font-smoothing:antialiased;
}
.mono{font-family:ui-monospace,"SF Mono","Cascadia Mono","JetBrains Mono",Menlo,Consolas,monospace;
  font-variant-numeric:tabular-nums}
.wrap{max-width:1500px;margin:0 auto;padding:28px 22px 80px}
h1{font-size:clamp(22px,3.2vw,30px);line-height:1.15;letter-spacing:-.02em;margin:0;text-wrap:balance}
.lede{color:var(--ink-2);max-width:66ch;margin:10px 0 0}
.eyebrow{font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted);
  font-weight:600;margin:0 0 8px}

header{border-bottom:1px solid var(--line);padding-bottom:22px;margin-bottom:22px}
.warn{
  display:flex;gap:10px;align-items:flex-start;margin-top:16px;padding:11px 14px;
  border:1px solid var(--hot);border-left-width:3px;border-radius:5px;
  background:var(--hot-soft);color:var(--ink);font-size:13.5px;max-width:78ch;
}
.warn b{color:var(--hot)}

.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:0 0 18px}
.seg{display:inline-flex;background:var(--sunk);border:1px solid var(--line);border-radius:7px;padding:3px;gap:2px}
.seg button{
  font:inherit;font-size:13px;font-weight:550;color:var(--ink-2);background:none;border:0;
  padding:6px 13px;border-radius:5px;cursor:pointer;white-space:nowrap;
}
.seg button:hover{color:var(--ink)}
.seg button[aria-pressed="true"]{background:var(--surface);color:var(--ink);box-shadow:var(--shadow)}
.seg button:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
.spacer{flex:1}
label.ctl{font-size:12.5px;color:var(--muted);display:inline-flex;gap:7px;align-items:center}
select,input[type=search]{
  font:inherit;font-size:13px;background:var(--surface);color:var(--ink);
  border:1px solid var(--line);border-radius:6px;padding:5px 8px;
}
select:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:1px}

.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(132px,1fr));gap:10px;margin-bottom:20px}
.tile{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:12px 13px}
.tile .k{font-size:10.5px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);font-weight:600}
.tile .v{font-size:21px;font-weight:600;letter-spacing:-.02em;margin-top:3px}
.tile .s{font-size:11.5px;color:var(--muted);margin-top:1px}
.tile.hotv .v{color:var(--hot)}

.panel{background:var(--surface);border:1px solid var(--line);border-radius:9px;box-shadow:var(--shadow);
  overflow:hidden;margin-bottom:18px}
.panel > h2{font-size:14px;margin:0;padding:12px 15px;border-bottom:1px solid var(--line-soft);
  letter-spacing:-.01em}
.panel .note{padding:11px 15px;font-size:12.5px;color:var(--muted);border-bottom:1px solid var(--line-soft)}
.scroll{overflow:auto;max-height:70vh}

table{border-collapse:collapse;width:100%;font-size:12.5px}
th,td{text-align:right;padding:5px 9px;border-bottom:1px solid var(--line-soft);white-space:nowrap}
th{position:sticky;top:0;background:var(--sunk);z-index:2;font-size:10.5px;letter-spacing:.07em;
  text-transform:uppercase;color:var(--muted);font-weight:600}
td:first-child,th:first-child{text-align:left}
tbody tr:hover td{background:var(--sunk)}

/* ── the matrix ───────────────────────────────────────────────── */
.mx{border-collapse:separate;border-spacing:0;font-size:11px}
.mx th{padding:4px 6px;text-align:center;font-size:10px}
.mx th.stepcol{position:sticky;left:0;z-index:3;text-align:right}
.mx td.stepcol{position:sticky;left:0;background:var(--sunk);z-index:1;text-align:right;
  color:var(--muted);font-size:10.5px;padding:2px 7px;border-right:1px solid var(--line)}
.mx td{padding:2px 4px;border-bottom:1px solid var(--line-soft);text-align:left;vertical-align:top}
.cell{display:flex;flex-wrap:wrap;gap:2px}
.e{padding:0 2px;border-radius:3px;color:var(--ink-2);cursor:default}
.e.miss{background:var(--hot-soft);color:var(--hot);font-weight:600}
.e.spec{background:var(--accent-soft);color:var(--accent);font-weight:600}
.e.rep{box-shadow:inset 0 -2px 0 var(--accent)}
.mbcell{text-align:right;font-size:10.5px}

.legend{display:flex;flex-wrap:wrap;gap:14px;padding:10px 15px;font-size:12px;color:var(--muted);
  border-bottom:1px solid var(--line-soft)}
.legend span{display:inline-flex;align-items:center;gap:6px}
.sw{width:11px;height:11px;border-radius:3px;display:inline-block}

.bars td.bar{width:100%;padding-right:14px}
.b{height:9px;background:var(--accent);border-radius:2px;min-width:2px;display:block}
.b.hot{background:var(--hot)}

canvas{display:block;width:100%;height:auto}
.pager{display:flex;gap:8px;align-items:center;padding:10px 15px;border-top:1px solid var(--line-soft);
  font-size:12.5px;color:var(--muted)}
.pager button{font:inherit;font-size:12.5px;background:var(--surface);color:var(--ink);
  border:1px solid var(--line);border-radius:6px;padding:4px 11px;cursor:pointer}
.pager button:disabled{opacity:.4;cursor:default}
.pager button:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
input[type=range]{accent-color:var(--accent);flex:1;max-width:340px}
footer{margin-top:34px;padding-top:18px;border-top:1px solid var(--line);color:var(--muted);font-size:12.5px}
a{color:var(--accent)}
.hide{display:none}
</style>

<div class="wrap">
<header>
  <p class="eyebrow">BigMoeOnEdge · bench-data / 2026-07-15</p>
  <h1>Route trace — which experts each layer actually picks</h1>
  <p class="lede">Every routed expert of three MoE models, captured on a OnePlus 15R with
  <code class="mono">--route-trace</code>. One row per step, layer and rank slot: the expert id, the
  weight the router gave it, and whether it was already in the cache.</p>
  <div class="warn"><span>⚠</span><div><b>These are not benchmark numbers.</b> Every run had the trace on,
  which costs a graph barrier per layer. The throughput figures are context for reading the routing —
  routing itself is deterministic and unaffected. Don't compare them with the tables in
  <code class="mono">benchmarks.md</code>.</div></div>
</header>

<div class="bar">
  <div class="seg" id="models"></div>
  <div class="spacer"></div>
  <div class="seg" id="views"></div>
</div>

<div class="tiles" id="tiles"></div>

<div id="view-matrix">
  <div class="panel">
    <h2>Miss density — the whole run at a glance</h2>
    <div class="note">Each column is a decode step, each row a layer. Amber = that layer routed an
      expert it had to read from flash. This is where the time goes.</div>
    <div style="padding:12px 15px"><canvas id="heat"></canvas></div>
  </div>

  <div class="panel">
    <h2>Step × layer matrix</h2>
    <div class="legend">
      <span><i class="sw" style="background:var(--hot-soft);border:1px solid var(--hot)"></i> miss — read from flash</span>
      <span><i class="sw" style="background:var(--sunk);border:1px solid var(--line)"></i> hit — already resident</span>
      <span><i class="sw" style="background:var(--accent-soft);border:1px solid var(--accent)"></i> prefetch hit</span>
      <span><i class="sw" style="box-shadow:inset 0 -2px 0 var(--accent);border:1px solid var(--line)"></i> also routed on the previous step</span>
    </div>
    <div class="bar" style="padding:10px 15px;margin:0">
      <label class="ctl">phase
        <select id="phase"><option value="1">decode</option><option value="0">prefill</option></select>
      </label>
      <label class="ctl">cells
        <select id="cellmode">
          <option value="experts">expert ids</option>
          <option value="weights">ids + routing %</option>
          <option value="mb">MiB read</option>
        </select>
      </label>
      <label class="ctl">layers
        <select id="layerspan"></select>
      </label>
      <div class="spacer"></div>
      <span class="ctl mono" id="steplabel"></span>
    </div>
    <div class="scroll" id="mxwrap"></div>
    <div class="pager">
      <button id="prev">← earlier</button>
      <input type="range" id="stepslider" min="0" value="0">
      <button id="next">later →</button>
      <span id="pagenote"></span>
    </div>
  </div>
</div>

<div id="view-hot" class="hide">
  <div class="panel">
    <h2>Hot experts per layer</h2>
    <div class="note">Decode only. <b>unique</b> is how many of the bank the layer touched at all;
      <b>top-8</b> is the share of activations its eight busiest experts carried. A layer with high
      unique and low top-8 routes near-uniformly — nothing worth preloading there.</div>
    <div class="scroll"><table id="hottab"></table></div>
  </div>
</div>

<div id="view-metrics" class="hide">
  <div class="panel">
    <h2>Per-token metrics</h2>
    <div class="note">The <code class="mono">--csv</code> sink, joined to the trace by step.
      <b>majflt</b> is major page faults — dense weights being demand-paged back in after the expert
      traffic evicted them.</div>
    <div class="scroll"><table id="mtab"></table></div>
  </div>
</div>

<div id="view-raw" class="hide">
  <div class="panel">
    <h2>Raw rows</h2>
    <div class="bar" style="padding:10px 15px;margin:0">
      <label class="ctl">filter
        <input type="search" id="rawq" placeholder="e.g. layer=12  expert=87  miss" size="26">
      </label>
      <div class="spacer"></div>
      <span class="ctl mono" id="rawcount"></span>
    </div>
    <div class="scroll"><table id="rawtab"></table></div>
    <div class="pager">
      <button id="rprev">←</button><span id="rpage" class="mono"></span><button id="rnext">→</button>
      <span class="spacer"></span>
      <span>Full-precision CSVs live in the repo — this viewer rounds weights to 4 decimals.</span>
    </div>
  </div>
</div>

<footer>
  Captured 2026-07-15 · OnePlus 15R (SM8845), 11.1 GB · engine <code class="mono">feat/route-trace</code>
  on <code class="mono">main@e7d2e8c</code> · device clocks were capped (policy0 2.19 of 3.32 GHz) and
  are recorded per run in the <code class="mono">.state</code> files.
</footer>
</div>

<script>
const DATA = __DATA__;
const TAGS = Object.keys(DATA);
let cur = TAGS[0], view = "matrix", phase = 1, cellmode = "experts", stepPage = 0, layerFrom = 0;
let rawPage = 0;
const STEP_WIN = 26, LAYER_WIN = 16, RAW_PAGE = 300;

// ── parse the packed cells once per model ────────────────────────
const CACHE = {};
function cells(tag){
  if (CACHE[tag]) return CACHE[tag];
  const m = new Map();
  for (const line of DATA[tag].cells.split("\n")){
    if (!line) continue;
    const i1 = line.indexOf("|"), i2 = line.indexOf("|", i1+1), i3 = line.indexOf("|", i2+1);
    const ph = +line.slice(0,i1), st = +line.slice(i1+1,i2), l = +line.slice(i2+1,i3);
    const rec = line.slice(i3+1).split(";").map(t=>{
      const p = t.split(":"); return {e:+p[0], w:+p[1]/10000, r:+p[2]};
    });
    m.set(ph+"|"+st+"|"+l, rec);
  }
  CACHE[tag] = m; return m;
}
function stepsOf(tag, ph){
  const s = new Set();
  for (const k of cells(tag).keys()) if (+k.split("|")[0]===ph) s.add(+k.split("|")[1]);
  return [...s].sort((a,b)=>a-b);
}

// ── chrome ───────────────────────────────────────────────────────
function seg(el, items, active, cb){
  el.innerHTML = "";
  for (const [val,label] of items){
    const b = document.createElement("button");
    b.textContent = label; b.setAttribute("aria-pressed", val===active);
    b.onclick = ()=>cb(val); el.appendChild(b);
  }
}
function fmt(n, d=0){ return n.toLocaleString(undefined,{minimumFractionDigits:d,maximumFractionDigits:d}); }

function tiles(){
  const d = DATA[cur], s = d.summary;
  const res = d.res, tot = res[0]+res[1]+res[2];
  const hit = tot ? 100*(res[1]+res[2])/tot : 0;
  const mets = d.metrics;
  const mf = mets.map(r=>+r[9]).filter(x=>!isNaN(x));
  const q = Math.max(1, Math.floor(mf.length/4));
  const mfQ1 = mf.slice(0,q).reduce((a,b)=>a+b,0)/q, mfQ4 = mf.slice(-q).reduce((a,b)=>a+b,0)/q;
  const t = [
    ["tok/s", (+s["tok/s"]).toFixed(3), "trace on — context only"],
    ["decode cache hit", hit.toFixed(1)+"%", fmt(tot)+" routings"],
    ["one expert", d.per_expert_mib+" MiB", "the unit the cache trades in"],
    ["touched this run", fmt(d.touched_mib)+" MiB", "of a "+fmt(d.bank_mib)+" MiB bank"],
    ["dense", fmt(d.dense_mib)+" MiB", "mmap-resident, never streamed"],
    ["compute / token", (+s["compute_s/tok"]).toFixed(3)+" s", "of "+(1/+s["tok/s"]).toFixed(3)+" s total"],
    ["majflt / token", fmt(Math.round(mfQ1))+" → "+fmt(Math.round(mfQ4)), "first quarter → last", mfQ4 > mfQ1*2],
  ];
  document.getElementById("tiles").innerHTML = t.map(([k,v,s2,hot])=>
    `<div class="tile${hot?" hotv":""}"><div class="k">${k}</div><div class="v mono">${v}</div><div class="s">${s2}</div></div>`
  ).join("");
}

// ── heatmap: layers x steps, miss density ────────────────────────
function heat(){
  const d = DATA[cur], c = document.getElementById("heat");
  const steps = stepsOf(cur, 1), layers = d.moe;
  const cw = Math.max(3, Math.min(9, Math.floor(1100/steps.length)));
  const ch = Math.max(4, Math.min(14, Math.floor(360/layers.length)));
  const dpr = window.devicePixelRatio||1;
  c.width = steps.length*cw*dpr; c.height = layers.length*ch*dpr;
  c.style.height = (layers.length*ch)+"px";
  const g = c.getContext("2d"); g.scale(dpr,dpr);
  const cs = getComputedStyle(document.documentElement);
  const hot = cs.getPropertyValue("--hot").trim(), sunk = cs.getPropertyValue("--sunk").trim();
  const M = cells(cur);
  steps.forEach((st,x)=>layers.forEach((l,y)=>{
    const rec = M.get("1|"+st+"|"+l); if(!rec) return;
    const miss = rec.filter(r=>r.r===0).length/rec.length;
    g.fillStyle = sunk; g.fillRect(x*cw,y*ch,cw,ch);
    if (miss>0){ g.globalAlpha = 0.18+0.82*miss; g.fillStyle = hot;
      g.fillRect(x*cw,y*ch,cw,ch); g.globalAlpha = 1; }
  }));
}

// ── matrix ───────────────────────────────────────────────────────
function matrix(){
  const d = DATA[cur], M = cells(cur);
  const steps = stepsOf(cur, phase);
  const layers = d.moe.slice(layerFrom, layerFrom+LAYER_WIN);
  const maxPage = Math.max(0, Math.ceil(steps.length/STEP_WIN)-1);
  stepPage = Math.min(stepPage, maxPage);
  const win = steps.slice(stepPage*STEP_WIN, stepPage*STEP_WIN+STEP_WIN);
  const eb = d.expert_bytes;

  let h = '<table class="mx mono"><thead><tr><th class="stepcol">step</th>';
  for (const l of layers) h += `<th>L${l}</th>`;
  h += "</tr></thead><tbody>";
  const prev = {};
  // seed "previous step" from the step before the window so the first row is honest
  const seedIdx = stepPage*STEP_WIN-1;
  if (seedIdx>=0) for (const l of layers){
    const r = M.get(phase+"|"+steps[seedIdx]+"|"+l); if(r) prev[l] = new Set(r.map(x=>x.e));
  }
  for (const st of win){
    h += `<tr><td class="stepcol">${st}</td>`;
    for (const l of layers){
      const rec = M.get(phase+"|"+st+"|"+l);
      if (!rec){ h += "<td></td>"; continue; }
      if (cellmode==="mb"){
        const mb = rec.filter(r=>r.r===0).length * (eb[l]||0)/1048576;
        h += `<td class="mbcell" style="color:${mb>0?"var(--hot)":"var(--muted)"}">${mb?mb.toFixed(1):"·"}</td>`;
      } else {
        const p = prev[l]||new Set();
        h += '<td><div class="cell">' + rec.map(r=>{
          const cls = "e"+(r.r===0?" miss":r.r===2?" spec":"")+(p.has(r.e)?" rep":"");
          const txt = cellmode==="weights" ? `${r.e}<span style="opacity:.6">·${Math.round(r.w*100)}</span>` : r.e;
          return `<span class="${cls}" title="expert ${r.e} · weight ${(r.w*100).toFixed(1)}% · ${["miss — read from flash","hit","prefetch hit"][r.r]}">${txt}</span>`;
        }).join("") + "</div></td>";
      }
      prev[l] = new Set(rec.map(x=>x.e));
    }
    h += "</tr>";
  }
  h += "</tbody></table>";
  document.getElementById("mxwrap").innerHTML = h;
  document.getElementById("steplabel").textContent =
    win.length ? `steps ${win[0]}–${win[win.length-1]} of ${steps.length}` : "no rows";
  document.getElementById("pagenote").textContent =
    `page ${stepPage+1} / ${maxPage+1} · ${d.k} experts per cell`;
  const sl = document.getElementById("stepslider");
  sl.max = maxPage; sl.value = stepPage;
  document.getElementById("prev").disabled = stepPage===0;
  document.getElementById("next").disabled = stepPage===maxPage;
}

function layerSpans(){
  const d = DATA[cur], sel = document.getElementById("layerspan");
  sel.innerHTML = "";
  for (let i=0;i<d.moe.length;i+=LAYER_WIN){
    const a = d.moe[i], b = d.moe[Math.min(i+LAYER_WIN-1, d.moe.length-1)];
    const o = document.createElement("option"); o.value = i; o.textContent = `L${a}–L${b}`;
    sel.appendChild(o);
  }
  sel.value = Math.min(layerFrom, d.moe.length-1);
}

// ── hot experts ──────────────────────────────────────────────────
function hotTab(){
  const d = DATA[cur];
  let h = "<thead><tr><th>layer</th><th>unique</th><th>top-8 share</th><th class='bar'>busiest experts — id·activations</th></tr></thead><tbody>";
  for (const l of d.moe){
    const hot = d.hot[l], uniq = d.uniq[l];
    const tot = hot.reduce((a,b)=>a+b[1],0);
    const all = Object.values(d.uniq); // for scale
    const top8 = hot.slice(0,8).reduce((a,b)=>a+b[1],0);
    const totalAct = (DATA[cur].metrics.length) * d.k;
    const share = 100*top8/ (d.k * stepsOf(cur,1).length);
    const max = hot[0] ? hot[0][1] : 1;
    const bars = hot.slice(0,10).map(([e,c])=>
      `<span style="display:inline-flex;flex-direction:column;gap:2px;width:58px;vertical-align:bottom">
         <span class="b${c/max>0.6?" hot":""}" style="width:${Math.max(6,100*c/max)}%"></span>
         <span style="font-size:10px;color:var(--muted)" class="mono">${e}·${c}</span></span>`).join(" ");
    h += `<tr><td class="mono">L${l}</td><td class="mono">${uniq} / ${d.n_expert}</td>
      <td class="mono">${share.toFixed(1)}%</td><td class="bar">${bars}</td></tr>`;
  }
  document.getElementById("hottab").innerHTML = h + "</tbody>";
}

// ── metrics ──────────────────────────────────────────────────────
function metTab(){
  const d = DATA[cur];
  const hdr = d.metrics_hdr;
  let h = "<thead><tr>" + hdr.map(x=>`<th>${x}</th>`).join("") + "</tr></thead><tbody>";
  for (const r of d.metrics){
    h += "<tr>" + r.map((v,i)=>{
      const isMaj = hdr[i]==="majflt";
      return `<td class="mono"${isMaj&&+v>1000?' style="color:var(--hot);font-weight:600"':""}>${v}</td>`;
    }).join("") + "</tr>";
  }
  document.getElementById("mtab").innerHTML = h + "</tbody>";
}

// ── raw rows ─────────────────────────────────────────────────────
function rawRows(){
  const d = DATA[cur], M = cells(cur), q = document.getElementById("rawq").value.trim().toLowerCase();
  const out = [];
  for (const [key, rec] of M){
    const [ph, st, l] = key.split("|").map(Number);
    for (let i=0;i<rec.length;i++){
      const r = rec[i];
      out.push([ph, st, l, i, r.e, r.w, r.r, r.r===0 ? (d.expert_bytes[l]||0) : 0]);
    }
  }
  let rows = out;
  if (q){
    rows = out.filter(r=>{
      const s = `phase=${r[0]?"decode":"prefill"} step=${r[1]} layer=${r[2]} slot=${r[3]} expert=${r[4]} ${["miss","hit","prefetch"][r[6]]}`;
      return q.split(/\s+/).every(t=>s.includes(t));
    });
  }
  const pages = Math.max(1, Math.ceil(rows.length/RAW_PAGE));
  rawPage = Math.min(rawPage, pages-1);
  const win = rows.slice(rawPage*RAW_PAGE, rawPage*RAW_PAGE+RAW_PAGE);
  let h = "<thead><tr><th>phase</th><th>step</th><th>layer</th><th>slot</th><th>expert</th><th>weight</th><th>residency</th><th>bytes read</th></tr></thead><tbody>";
  for (const r of win){
    const rn = ["miss","hit","prefetch"][r[6]];
    h += `<tr><td>${r[0]?"decode":"prefill"}</td><td class="mono">${r[1]}</td><td class="mono">${r[2]}</td>
      <td class="mono">${r[3]}</td><td class="mono" style="font-weight:600">${r[4]}</td>
      <td class="mono">${r[5].toFixed(4)}</td>
      <td class="mono"${r[6]===0?' style="color:var(--hot)"':r[6]===2?' style="color:var(--accent)"':""}>${rn}</td>
      <td class="mono">${r[7]?fmt(r[7]):"·"}</td></tr>`;
  }
  document.getElementById("rawtab").innerHTML = h + "</tbody>";
  document.getElementById("rawcount").textContent = fmt(rows.length)+" rows";
  document.getElementById("rpage").textContent = ` ${rawPage+1} / ${pages} `;
  document.getElementById("rprev").disabled = rawPage===0;
  document.getElementById("rnext").disabled = rawPage>=pages-1;
}

// ── render ───────────────────────────────────────────────────────
function render(){
  seg(document.getElementById("models"), TAGS.map(t=>[t, DATA[t].label]), cur, t=>{
    cur = t; stepPage = 0; layerFrom = 0; rawPage = 0; layerSpans(); render();
  });
  seg(document.getElementById("views"),
      [["matrix","Matrix"],["hot","Hot experts"],["metrics","Per-token"],["raw","Raw rows"]],
      view, v=>{ view = v; render(); });
  for (const v of ["matrix","hot","metrics","raw"])
    document.getElementById("view-"+v).classList.toggle("hide", v!==view);
  tiles();
  if (view==="matrix"){ heat(); matrix(); }
  if (view==="hot") hotTab();
  if (view==="metrics") metTab();
  if (view==="raw") rawRows();
}

document.getElementById("phase").onchange = e=>{ phase = +e.target.value; stepPage = 0; matrix(); };
document.getElementById("cellmode").onchange = e=>{ cellmode = e.target.value; matrix(); };
document.getElementById("layerspan").onchange = e=>{ layerFrom = +e.target.value; matrix(); };
document.getElementById("prev").onclick = ()=>{ stepPage--; matrix(); };
document.getElementById("next").onclick = ()=>{ stepPage++; matrix(); };
document.getElementById("stepslider").oninput = e=>{ stepPage = +e.target.value; matrix(); };
document.getElementById("rawq").oninput = ()=>{ rawPage = 0; rawRows(); };
document.getElementById("rprev").onclick = ()=>{ rawPage--; rawRows(); };
document.getElementById("rnext").onclick = ()=>{ rawPage++; rawRows(); };
matchMedia("(prefers-color-scheme:dark)").addEventListener("change", ()=>{ if(view==="matrix") heat(); });
new MutationObserver(()=>{ if(view==="matrix") heat(); })
  .observe(document.documentElement, {attributes:true, attributeFilter:["data-theme"]});

layerSpans();
render();
</script>
"""

html = HTML.replace("__DATA__", DATA_JSON)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(html)
print("wrote %s — %.1f KB" % (OUT, os.path.getsize(OUT) / 1024))
