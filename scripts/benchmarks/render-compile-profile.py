#!/usr/bin/env python3
"""Render offline CPU heatmaps and flamegraphs from compile profile summaries.

uv run --no-project python scripts/benchmarks/render-compile-profile.py \
  --summary Baseline=/tmp/run/summary.json --output /tmp/profile.html

Companion samples.jsonl supplies complete stacks. Without it, omitted summary
entries remain an explicit bucket. All percentages use all sampled CPUs.
"""

import argparse
from collections import Counter, defaultdict
import fnmatch
import hashlib
import html
import json
from pathlib import Path
import runpy
import shutil


def integer(value):
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"invalid sample count: {value!r}")
    return value


def add_path(root, path, count):
    root["value"] += count
    for name in path:
        root = root["children"].setdefault(name, node(name))
        root["value"] += count


def node(name):
    return {"name": name, "value": 0, "children": {}}


def finish_tree(tree):
    children = list(tree["children"].values())
    if sum(child["value"] for child in children) > tree["value"]:
        raise ValueError("flamegraph children exceed parent samples")
    return {**tree, "children": [finish_tree(child) for child in children]}


def tool_path(name, cross_name=None):
    repo = Path(__file__).resolve().parents[2]
    choices = [shutil.which(name), f"/opt/homebrew/opt/llvm/bin/{name}"]
    if cross_name:
        choices.append(str(repo / "compilers/dir/bin" / cross_name))
    return next((str(p) for p in choices if p and Path(p).is_file()), None)


def raw_profiles(path, summary, helpers, args, warnings):
    records = []
    digest = hashlib.sha256()
    addresses = defaultdict(set)
    ranges = summary.get("module_ranges", {})

    def locate(address):
        for name, region in ranges.items():
            if region["base"] <= address < region["end"]:
                return name, address - region["base"]
        return "kernel", address

    with path.open("rb") as stream:
        for line in stream:
            if not line.endswith(b"\n"):
                warnings.append("Incomplete last sample record ignored.")
                break
            digest.update(line)
            record = json.loads(line)
            cpus = list(helpers["registers"](record["registers"]))
            records.append((record, cpus))
            for cpu in cpus:
                if cpu.get("CPL") == 0:
                    name, address = locate(cpu["RIP"])
                    addresses[name].add(address)
            for stack in record.get("stacks", []):
                for index, ip in enumerate(stack.get("ips", [])):
                    name, address = locate(ip - bool(index))
                    addresses[name].add(address)

    tables = {}
    for name, wanted in addresses.items():
        provenance = summary.get("symbol_files", {}).get(name, {})
        symbol_path = Path(provenance.get("path", ""))
        if not symbol_path.is_file() or not args.nm:
            warnings.append(f"{name}: symbol file/tool unavailable; addresses retained.")
            continue
        table = helpers["Symbols"](symbol_path, args.nm)
        if table.sha256 != provenance.get("sha256"):
            raise ValueError(f"symbol SHA256 mismatch: {symbol_path}")
        if args.addr2line:
            table.resolve_dwarf({a for a in wanted if table.lookup(a) is None}, args.addr2line)
        tables[name] = table

    def symbol(address):
        name, relative = locate(address)
        label = tables[name].lookup(relative) if name in tables else None
        return f"{name}:{label}" if label else f"{name}+0x{relative:x} [unknown]"

    phases = {}
    for record, cpus in records:
        phase = phases.setdefault(record["phase"], {
            "total": 0, "snapshots": 0, "states": Counter(), "leaves": Counter(),
            "paths": Counter(), "stops": Counter(), "modes": Counter(),
            "classification_stops": Counter(), "omitted_callers": 0,
            "invalid_callers": 0, "unverified_callers": 0, "synthetic_roots": 0,
            "stack_samples": 0, "with_callers": 0, "query_wall_s": 0,
            "paused_snapshots": 0, "paused_wall_s": 0,
            "stop_query_wall_s": 0, "resume_query_wall_s": 0,
        })
        phase["snapshots"] += 1
        for key in ("query_wall_s", "paused_wall_s", "stop_query_wall_s", "resume_query_wall_s"):
            phase[key] += record.get(key, 0)
        phase["paused_snapshots"] += bool(record.get("paused"))
        stacks = {stack["cpu"]: stack for stack in record.get("stacks", [])}
        for cpu in cpus:
            phase["total"] += 1
            state = ("halted" if cpu.get("HLT") else "kernel_running" if cpu.get("CPL") == 0
                     else "user_running" if cpu.get("CPL") == 3 else "unknown_running")
            phase["states"][state] += 1
            if state != "kernel_running":
                continue
            leaf = symbol(cpu["RIP"])
            phase["leaves"][leaf] += 1
            stack = stacks.get(cpu["cpu"])
            if not stack or not stack.get("ips"):
                phase["paths"][("No stack captured", leaf)] += 1
                continue
            if stack["ips"][0] != cpu["RIP"]:
                phase["paths"][("Stack/register mismatch; leaf only", leaf)] += 1
                phase["stops"]["stack-register-mismatch"] += 1
                continue
            reason = stack.get("stop_reason", "unspecified")
            mode = "paused" if record.get("paused") else "asynchronous"
            phase["modes"][mode] += 1
            phase["stops"][reason] += 1
            phase["stack_samples"] += 1
            classified = helpers["classify_stack"](stack["ips"], tables, ranges)
            frames = classified["symbols"]
            phase["with_callers"] += sum(not f.startswith("[synthetic thread root:") for f in frames) > 1
            for key in ("omitted_callers", "invalid_callers", "unverified_callers", "synthetic_roots"):
                phase[key] += classified[key]
            classification = classified["stop_reason"]
            phase["classification_stops"][classification] += 1
            boundary = classification if classification != "complete" else reason
            phase["paths"][(f"Observed stack ({mode}; stop: {boundary})", *reversed(frames))] += 1
    return phases, digest.hexdigest()


def summary_profile(profile):
    total = integer(profile.get("cpu_samples", 0))
    states = Counter({k: integer(v) for k, v in profile.get("states", {}).items()})
    leaves = Counter()
    for entry in profile.get("kernel_running_symbols", []):
        leaves[entry["symbol"]] += integer(entry["count"])
    paths = Counter()
    for entry in profile.get("stack_chains", []) if "stack_classification_stops" in profile else []:
        frames = entry["chain"].split(" <- ")
        paths[("Summary stack (outer callers may be missing)", *reversed(frames))] += integer(entry["count"])
    return {"total": total, "states": states, "leaves": leaves, "paths": paths,
            "stops": profile.get("stack_stop_reasons", {}), "modes": {"summary": sum(paths.values())},
            "stack_samples": profile.get("stack_samples", 0),
            "with_callers": profile.get("stacks_with_callers", 0),
            "classification_stops": profile.get("stack_classification_stops", {}),
            **{k: profile.get("stack_" + k, 0) for k in (
                "omitted_callers", "invalid_callers", "unverified_callers", "synthetic_roots")},
            **{k: profile.get(k, 0) for k in ("snapshots", "query_wall_s", "paused_snapshots", "paused_wall_s")}}


def prepare_column(label, name, metrics, profile):
    total, states = profile["total"], profile["states"]
    if sum(states.values()) != total:
        raise ValueError(f"{label}/{name}: CPU state counts do not sum to total")
    kernel = states.get("kernel_running", 0)
    leaves = profile["leaves"]
    missing_leaves = kernel - sum(leaves.values())
    missing_stacks = kernel - sum(profile["paths"].values())
    if min(missing_leaves, missing_stacks) < 0:
        raise ValueError(f"{label}/{name}: kernel detail exceeds kernel CPU samples")
    if missing_leaves:
        leaves["Kernel leaves omitted by summary"] += missing_leaves
    tree = node("All CPU samples")
    for frames, count in profile["paths"].items():
        add_path(tree, ("Kernel running", *frames), count)
    if missing_stacks:
        add_path(tree, ("Kernel running", "Stacks omitted or not captured"), missing_stacks)
    names = {"halted": "Halted", "user_running": "User mode (not symbolized)"}
    for state, count in states.items():
        if state != "kernel_running":
            add_path(tree, (names.get(state, state),), count)
    if tree["value"] != total:
        raise ValueError(f"{label}/{name}: flamegraph does not conserve samples")
    return {"arm": label, "phase": name, "total": total, "states": states,
            "leaves": leaves, "tree": finish_tree(tree), "metrics": metrics,
            "profile": {k: v for k, v in profile.items() if k not in ("paths", "leaves", "states")},
            "missing_leaves": missing_leaves, "missing_stacks": missing_stacks}


def load_run(label, path, args, helpers):
    summary = json.loads(path.read_text())
    if not isinstance(summary.get("phases"), dict):
        raise ValueError(f"{path}: expected summarize-compile.py output, not runner report")
    directory = Path(summary.get("directory", path.parent))
    report_path = directory / "report.json"
    report = json.loads(report_path.read_text()) if report_path.is_file() else {}
    samples = directory / "samples.jsonl"
    warnings = []
    raw, samples_hash = (raw_profiles(samples, summary, helpers, args, warnings)
                         if samples.is_file() and not args.summary_only else ({}, None))
    if not raw:
        warnings.append("Unclassified legacy summary stacks are hidden; classified summary chains may be truncated.")
    columns = []
    for name in dict.fromkeys([*summary["phases"], *raw]):
        if not any(fnmatch.fnmatchcase(name, pattern) for pattern in args.phase):
            continue
        value = summary["phases"].get(name, {})
        profile = raw.get(name) or summary_profile(value.get("profile", {}))
        columns.append(prepare_column(label, name, {k: v for k, v in value.items() if k != "profile"}, profile))
    return {"label": label, "summary_path": str(path.resolve()),
            "summary_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "samples_path": str(samples) if raw else None,
            "samples_sha256": samples_hash,
            "samples_hash_scope": "complete JSONL records consumed",
            "source": "complete raw samples" if raw else "summary (may be truncated)",
            "warnings": warnings, "columns": columns,
            "metadata": {k: v for k, v in summary.items() if k != "phases"},
            "run": {k: v for k, v in report.items() if k not in (
                "phases", "failure_blocks", "registers", "irq", "incomplete_phase")}}


def heatmap_svg(runs, path, limit):
    columns = [column for run in runs for column in run["columns"]]
    totals = Counter()
    for column in columns:
        totals.update(column["leaves"])
    names = [name for name, _ in totals.most_common(limit)]
    rows = [("User mode", "user_running"), ("Halted", "halted"),
            ("Unknown state", "unknown_running"), *((name, None) for name in names),
            ("Other kernel functions", "other")]
    label_width, cell_width, row_height, top = 640, 154, 27, 142
    width, height = label_width + cell_width * len(columns) + 20, top + row_height * len(rows) + 66
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}" role="img">',
             '<title>Compilation CPU heatmap</title>',
             '<desc>Exclusive function samples, divided by all CPU samples in each phase. '
             'User mode, halted time and omitted kernel functions remain in the totals.</desc>',
             f'<rect width="{width}" height="{height}" fill="#f7f9fb"/>']

    def text(x, y, value, size=12, fill="#152332", anchor="start"):
        parts.append(f'<text x="{x}" y="{y}" font-family="sans-serif" font-size="{size}" '
                     f'fill="{fill}" text-anchor="{anchor}">{html.escape(str(value))}</text>')

    text(18, 32, "Compilation CPU profile · exclusive samples", 23)
    text(18, 57, "Each column uses all CPU samples. Orange intensity shows frequency, not exact elapsed time.", 13)
    text(18, 80, "Function / execution state", 12)
    for index, column in enumerate(columns):
        x = label_width + index * cell_width + cell_width / 2
        text(x, 79, column["arm"], 12, anchor="middle")
        text(x, 98, column["phase"], 12, anchor="middle")
        seconds = column["metrics"].get("guest_wall_s")
        text(x, 117, f'n={column["total"]} · rc={column["metrics"].get("rc", "?")}' + (f' · {seconds:.2f}s' if seconds is not None else ""),
             11, "#566678", "middle")
    for index, (name, kind) in enumerate(rows):
        y = top + index * row_height
        parts.append(f'<g><title>{html.escape(name)}</title>')
        text(18, y + 18, name if len(name) < 85 else name[:82] + "…")
        for col, column in enumerate(columns):
            count = (column["states"].get(kind, 0) if kind and kind != "other" else
                     sum(n for key, n in column["leaves"].items() if key not in names) if kind == "other" else
                     column["leaves"].get(name, 0))
            share = count / column["total"] if column["total"] else 0
            alpha = min(.9, share ** .5 * 2.2)
            color = "#e3ecf3" if kind and kind != "other" else "#%02x%02x%02x" % tuple(
                round(255 * (1 - alpha) + value * alpha) for value in (203, 95, 22))
            x = label_width + col * cell_width
            parts.append(f'<rect x="{x}" y="{y}" width="{cell_width-2}" height="{row_height-2}" fill="{color}">'
                         f'<title>{count} / {column["total"]} CPU samples</title></rect>')
            text(x + cell_width / 2, y + 18, f"{share*100:.2f}%" if column["total"] else "No samples",
                 12, "white" if not kind and alpha > .6 else "#152332", "middle")
        parts.append("</g>")
    text(18, height - 30, "All rows sum to 100% per sampled column. Full symbols, stacks, limitations and run metadata are in the companion HTML.", 12)
    parts.append("</svg>")
    path.write_text("\n".join(parts))


HTML = r'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pedigree compilation CPU profile</title>
<style>
:root{font:14px system-ui,sans-serif;color:#152332;background:#f5f6f8}body{margin:0;padding:28px;max-width:1700px}h1{font-size:28px;margin:0 0 8px}h2{margin:28px 0 10px;font-size:20px}p{max-width:1100px;line-height:1.55}small,.muted{color:#566678}button,select,input{font:inherit;padding:7px 10px;border:1px solid #aebbc8;border-radius:4px;background:white;color:inherit}button{cursor:pointer}label{margin-right:12px}.controls{display:flex;align-items:center;flex-wrap:wrap;gap:10px;margin:12px 0}.panel{background:white;border:1px solid #d4dce4;border-radius:5px;padding:16px}.scroll{overflow:auto;max-height:650px}table{border-collapse:collapse;font-size:12px;width:100%}th,td{padding:7px 10px;border-bottom:1px solid #e0e6ed;text-align:right;min-width:98px;font-variant-numeric:tabular-nums}th:first-child,td:first-child{text-align:left;position:sticky;left:0;background:white;max-width:500px;min-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}thead th{position:sticky;top:0;background:#eaf0f5;z-index:2}thead th:first-child{z-index:3}.state td:first-child{font-weight:650}.sub{display:block;font-size:11px;color:#506070;margin-top:4px}.bar{display:flex;height:28px;border-radius:3px;overflow:hidden;margin:12px 0}.bar span{min-width:0}.legend{display:flex;gap:18px;flex-wrap:wrap}.legend i{display:inline-block;width:12px;height:12px;margin-right:5px}.detail{font-family:ui-monospace,monospace;font-size:12px;white-space:pre-wrap;overflow-wrap:anywhere;max-height:450px;overflow:auto;background:#f0f3f6;padding:12px}.flame{overflow:auto}svg{min-width:900px;display:block;width:100%;font:11px ui-monospace,monospace}svg rect{stroke:white;stroke-width:.7}svg g{cursor:pointer}svg g:hover rect{stroke:#142637;stroke-width:2}#hover{min-height:44px;overflow-wrap:anywhere;line-height:1.5}.notice{border-left:4px solid #d78b22;padding-left:12px}a{color:#12557e}.summary{display:flex;gap:24px;flex-wrap:wrap;margin:8px 0}.summary strong{font-size:19px;display:block}details{margin-top:18px}summary{cursor:pointer;font-weight:600}.empty{padding:30px;color:#667}
</style>
<h1>Where compilation spends CPU time</h1>
<p>Flat samples show which function was executing. The flamegraph groups observed callers from outer to inner frames. Percentages always use <strong>all CPU samples in that phase</strong>, including user execution and halted CPUs. Sampling shows frequency of observations, not exact time accounting.</p>
<div class="controls"><label>Find function <input id="search" type="search" placeholder="mmap, Spinlock, unknown…"></label><label>Heatmap rows <select id="rows"><option>25</option><option selected>40</option><option>80</option><option value="100000">All</option></select></label></div>
<h2>CPU heatmap · exclusive samples</h2>
<p class="muted">Functions × build arms and phases. Darker orange means a larger share of all sampled CPUs. User mode and halted time are separate rows. Hover a cell for its exact count.</p>
<div class="panel scroll"><table id="heat"></table></div>
<h2>Kernel flamegraph</h2>
<div class="controls"><label>Arm and phase <select id="phase"></select></label><button id="reset">Reset zoom</button></div>
<div class="panel"><div id="stats" class="summary"></div><div id="statebar" class="bar"></div><div id="legend" class="legend"></div><p id="quality" class="notice"></p><div id="hover">Hover a frame for its sample count. Click a frame to zoom; percentages retain the full-phase denominator.</div><div id="flame" class="flame"></div></div>
<details open><summary>Sampling and stack coverage</summary><div id="coverage" class="detail"></div></details>
<details><summary>Run metadata and source provenance</summary><div id="metadata" class="detail"></div></details>
<script id="profile-data" type="application/json">__DATA__</script>
<script>
"use strict";
const DATA=JSON.parse(document.getElementById('profile-data').textContent);
const columns=DATA.runs.flatMap(r=>r.columns), el=id=>document.getElementById(id);
const stateNames={kernel_running:'Kernel running',user_running:'User mode',halted:'Halted',unknown_running:'Unknown state'};
const stateColors={kernel_running:'#c96728',user_running:'#4286b5',halted:'#8c9d9e',unknown_running:'#886597'};
const pct=(n,t)=>t?(100*n/t).toFixed(2)+'%':'—';
const seconds=x=>typeof x==='number'?x.toFixed(3)+' s':'unavailable';
function cell(row,tag,value){const c=document.createElement(tag);c.textContent=value;row.append(c);return c;}
function heat(){
 const table=el('heat');table.replaceChildren();const head=table.createTHead(),r=head.insertRow();cell(r,'th','Sampled function / state');
 columns.forEach(c=>{const h=cell(r,'th',c.arm);const a=document.createElement('span');a.className='sub';a.textContent=c.phase+' · n='+c.total+' · rc='+(c.metrics.rc??'?');h.append(a);});
 const body=table.createTBody(), query=el('search').value.toLowerCase();
 const totals={};columns.forEach(c=>Object.entries(c.leaves).forEach(([s,n])=>totals[s]=(totals[s]||0)+n));
 const names=Object.keys(totals).filter(n=>n.toLowerCase().includes(query)).sort((a,b)=>totals[b]-totals[a]);
 const shown=names.slice(0,Number(el('rows').value));
 const rows=[...Object.entries(stateNames).filter(([k])=>k!=='kernel_running').map(([key,name])=>({name,key,state:true})),...shown.map(name=>({name})),{name:'Other kernel functions (outside displayed rows)',other:true}];
 rows.forEach(item=>{const tr=body.insertRow();if(item.state)tr.className='state';const h=cell(tr,'td',item.name);h.title=item.name;
  columns.forEach(c=>{const n=item.state?(c.states[item.key]||0):item.other?Object.entries(c.leaves).filter(([s])=>!shown.includes(s)).reduce((a,[,v])=>a+v,0):(c.leaves[item.name]||0);const td=cell(tr,'td',pct(n,c.total));td.title=n+' / '+c.total+' CPU samples';const alpha=c.total?Math.min(.9,Math.sqrt(n/c.total)*2.2):0;td.style.background=item.state?(n?stateColors[item.key]+'22':'#fafafa'):'rgba(203,95,22,'+alpha+')';if(!item.state&&alpha>.6)td.style.color='white';});
 });
 const tr=body.insertRow();cell(tr,'td','All CPU samples');columns.forEach(c=>cell(tr,'td',c.total?'100.00%':'No samples'));
}
const NS='http://www.w3.org/2000/svg';let selected=0,zoom=null;
function svgNode(tag,attrs){const x=document.createElementNS(NS,tag);Object.entries(attrs||{}).forEach(([k,v])=>x.setAttribute(k,v));return x;}
function color(name){if(name==='All CPU samples')return '#d9e1e8';if(name.startsWith('User mode'))return '#73acd0';if(name==='Halted')return '#abb7b7';if(/missing|omitted|captured|mismatch|Observed stack|Summary stack|synthetic thread root/.test(name))return '#c7c8c3';let hash=0;for(const ch of name)hash=(hash*31+ch.charCodeAt(0))>>>0;return 'hsl('+(15+hash%36)+',78%,'+(61+hash%13)+'%)';}
function depth(n){return 1+Math.max(0,...n.children.map(depth));}
function flame(){
 const c=columns[selected];if(!c)return;const root=zoom||c.tree,box=el('flame');box.replaceChildren();if(!c.total){box.textContent='No CPU samples captured for this phase.';return;}
 const height=depth(root)*25+12,svg=svgNode('svg',{viewBox:'0 0 1400 '+height,role:'img','aria-label':'CPU flamegraph for '+c.arm+' '+c.phase});box.append(svg);const query=el('search').value.toLowerCase();
 function draw(n,x,w,d){if(w<.45)return;const y=height-(d+1)*25,g=svgNode('g',{'data-name':n.name});const rect=svgNode('rect',{x,y,width:w,height:23,fill:color(n.name)});if(query&&!n.name.toLowerCase().includes(query))rect.setAttribute('opacity','.27');g.append(rect);const title=svgNode('title');title.textContent=n.name+'\n'+n.value+' / '+c.total+' all CPU samples ('+pct(n.value,c.total)+')';g.append(title);if(w>24){const text=svgNode('text',{x:x+4,y:y+16});const limit=Math.floor((w-8)/6.4);text.textContent=n.name.length>limit?n.name.slice(0,Math.max(0,limit-1))+'…':n.name;g.append(text);}g.addEventListener('mouseenter',()=>{el('hover').textContent=n.name+' — '+n.value+' / '+c.total+' CPU samples ('+pct(n.value,c.total)+'). '+(n.value-n.children.reduce((a,b)=>a+b.value,0))+' samples end at this frame.';});g.addEventListener('click',()=>{zoom=n;flame();});svg.append(g);let offset=x;n.children.slice().sort((a,b)=>a.name.localeCompare(b.name)).forEach(ch=>{const width=w*ch.value/n.value;draw(ch,offset,width,d+1);offset+=width;});}
 draw(root,0,1400,0);
}
function select(){
 selected=Number(el('phase').value);zoom=null;const c=columns[selected];if(!c)return;el('stats').replaceChildren();const items=[['CPU samples',c.total],['Guest wall',seconds(c.metrics.guest_wall_s)],['User / system',seconds(c.metrics.user_s)+' / '+seconds(c.metrics.system_s)],['Exit status',c.metrics.rc??'unavailable']];items.forEach(([name,value])=>{const d=document.createElement('div'),s=document.createElement('strong');s.textContent=value;d.append(s,document.createTextNode(name));el('stats').append(d);});
 el('statebar').replaceChildren();el('legend').replaceChildren();Object.entries(c.states).forEach(([state,count])=>{const span=document.createElement('span');span.style.width=(c.total?100*count/c.total:0)+'%';span.style.background=stateColors[state]||'#886597';span.title=(stateNames[state]||state)+': '+pct(count,c.total);el('statebar').append(span);const item=document.createElement('span'),icon=document.createElement('i');icon.style.background=span.style.background;item.append(icon,document.createTextNode((stateNames[state]||state)+' '+pct(count,c.total)));el('legend').append(item);});
 const p=c.profile,asyncCount=p.modes.asynchronous||0;el('quality').textContent=(asyncCount?'Asynchronous stacks can mix frames from different moments; treat caller relationships as provisional. ':'')+'Each stack begins at its outermost observed caller; missing older callers and depth limits are not reconstructed. '+(c.missing_stacks?c.missing_stacks+' kernel samples have no displayed stack. ':'')+'Invalid and unverifiable caller addresses are excluded. Synthetic thread roots are markers, not executed functions. User PCs are not symbolized.';
 el('coverage').textContent=JSON.stringify({arm:c.arm,phase:c.phase,all_cpu_samples:c.total,kernel_cpu_samples:c.states.kernel_running||0,stack_samples:p.stack_samples,stacks_with_callers:p.with_callers,stack_modes:p.modes,stop_reasons:p.stops,classification_stops:p.classification_stops,omitted_caller_frames:p.omitted_callers,invalid_caller_boundaries:p.invalid_callers,unverified_caller_boundaries:p.unverified_callers,synthetic_thread_roots:p.synthetic_roots,summary_omitted_leaf_samples:c.missing_leaves,summary_omitted_or_absent_stacks:c.missing_stacks,snapshots:p.snapshots,paused_snapshots:p.paused_snapshots,paused_wall_s:p.paused_wall_s,query_wall_s:p.query_wall_s,stop_query_wall_s:p.stop_query_wall_s,resume_query_wall_s:p.resume_query_wall_s,host_wall_s:c.metrics.host_wall_s,pause_share_of_host_wall:c.metrics.host_wall_s?pct(p.paused_wall_s,c.metrics.host_wall_s):'unavailable',overhead_note:'Paused time is stop acknowledgement to resume request. QMP stop/resume request latency is separate; asynchronous query duration is not paused time. Reported guest and host timings have not had profiler overhead subtracted.'},null,2);flame();
}
columns.forEach((c,i)=>{const o=document.createElement('option');o.value=i;o.textContent=c.arm+' · '+c.phase;el('phase').append(o);});
el('metadata').textContent=JSON.stringify({generated_by:DATA.generated_by,runs:DATA.runs.map(({columns,...rest})=>rest)},null,2);
el('phase').addEventListener('change',select);el('reset').addEventListener('click',()=>{zoom=null;flame();});el('search').addEventListener('input',()=>{heat();flame();});el('rows').addEventListener('change',heat);
el('phase').value=Math.max(0,columns.findIndex(c=>c.phase.startsWith('compile')));heat();select();
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", action="append", required=True, metavar="LABEL=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--heatmap-svg", type=Path, help="Also export a static heatmap")
    parser.add_argument("--heatmap-rows", type=int, default=30)
    parser.add_argument("--phase", action="append", help="Phase glob, repeatable; default: all")
    parser.add_argument("--summary-only", action="store_true", help="Do not read companion samples/symbols")
    parser.add_argument("--nm", default=tool_path("llvm-nm", "x86_64-pedigree-nm") or shutil.which("nm"))
    parser.add_argument("--addr2line", default=tool_path("llvm-addr2line", "x86_64-pedigree-addr2line"))
    args = parser.parse_args()
    if args.heatmap_rows < 1:
        parser.error("heatmap-rows must be positive")
    args.phase = args.phase or ["*"]
    helpers = runpy.run_path(str(Path(__file__).with_name("summarize-compile.py")))
    runs, labels = [], set()
    for specification in args.summary:
        label, separator, path = specification.partition("=")
        if not separator or not label or label in labels:
            parser.error("each --summary must have a unique nonempty LABEL=PATH")
        labels.add(label)
        runs.append(load_run(label, Path(path), args, helpers))
    if not any(run["columns"] for run in runs):
        parser.error("no phases matched")
    data = json.dumps({"generated_by": "render-compile-profile.py", "runs": runs}, ensure_ascii=True)
    data = data.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    args.output.write_text(HTML.replace("__DATA__", data))
    if args.heatmap_svg:
        heatmap_svg(runs, args.heatmap_svg, args.heatmap_rows)
    samples = sum(column["total"] for run in runs for column in run["columns"])
    print(f"Wrote {args.output.resolve()} ({samples} CPU samples; all state and flame totals checked)")


if __name__ == "__main__":
    main()
