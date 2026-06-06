#!/usr/bin/env python3
"""
AeVG SVG rendering conformance — side-by-side comparison against librsvg.

For each SVG in the corpus, renders three ways and reports per-pixel MAE
(Mean Absolute Error, 0=identical .. 255=max) of each AeVG column vs librsvg:

  Reference  — librsvg (rsvg-convert), the "correct" answer.
  Loader     — AeVG loader.load_svg → canvas → PNG (the render pipeline).
  Transpiled — SVG → AeVG vg{} source → compile → render → PNG (transpiler
               fidelity). Requires the transpiler to emit compilable vg{} source
               and a per-file build; skipped (column blank) if --no-transpile or
               the build fails.

Produces a self-contained HTML report (sortable, worst-first) + CSVs.

Adapted from cosyne/test/svg-compare.py (the TS CVG harness). The corpus is the
same 208-file W3C/CVG set; default location is the cosyne test/svg dir.

Usage:
  python3 aevg/test/svg-compare-aevg.py                 # all SVGs, loader only
  python3 aevg/test/svg-compare-aevg.py 410.svg heart   # specific files
  python3 aevg/test/svg-compare-aevg.py --transpile     # also build+render transpiled
  python3 aevg/test/svg-compare-aevg.py --limit 20      # first 20 (quick smoke)
  python3 aevg/test/svg-compare-aevg.py --svg-dir DIR   # custom corpus dir
"""

import sys
import os
import base64
import html as html_mod
import argparse
import subprocess
import tempfile
from pathlib import Path
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent           # aevg/test
AEVG_ROOT = SCRIPT_DIR.parent.parent                   # repo root (aether-ui)
RENDER_BIN = AEVG_ROOT / 'build' / 'svg_render'
BUILD_SH = AEVG_ROOT / 'build.sh'

# Default corpus: the cosyne 208-file W3C/CVG set. Override with --svg-dir.
DEFAULT_SVG_DIR = Path.home() / 'scm' / 'tsyne' / 'tsyne' / 'cosyne' / 'test' / 'svg'
DEFAULT_OUTPUT = SCRIPT_DIR / 'screenshots' / 'svg-compare-aevg'

SIZE = 400


def render_reference(svg_path: Path, output_path: Path):
    """Render SVG with rsvg-convert (librsvg) — the reference. White bg, square."""
    tmp = output_path.with_suffix('.tmp.png')
    subprocess.run(
        ['rsvg-convert', '-w', str(SIZE), '-h', str(SIZE),
         '--keep-aspect-ratio', '-b', 'white', '-o', str(tmp), str(svg_path)],
        check=True, capture_output=True,
    )
    img = Image.open(tmp).convert('RGBA')
    if img.size != (SIZE, SIZE):
        canvas = Image.new('RGBA', (SIZE, SIZE), (255, 255, 255, 255))
        canvas.paste(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
        canvas.save(output_path)
    else:
        img.save(output_path)
    img.close()
    tmp.unlink(missing_ok=True)


def _composite_white(png_path: Path) -> Image.Image:
    """Load a PNG and composite over white (AeVG renders on transparent bg,
    librsvg on white — match them before diffing)."""
    o = Image.open(png_path).convert('RGBA')
    bg = Image.new('RGBA', o.size, (255, 255, 255, 255))
    bg.alpha_composite(o)
    return bg.convert('RGB')


def render_loader(svg_path: Path, output_path: Path) -> bool:
    """Render SVG via the AeVG loader binary (stdin SVG, env out+size)."""
    if not RENDER_BIN.exists():
        return False
    env = os.environ.copy()
    env['AETHER_UI_HEADLESS'] = '1'
    env['AEVG_OUT'] = str(output_path)
    env['AEVG_SIZE'] = str(SIZE)
    try:
        svg_bytes = svg_path.read_bytes()
        r = subprocess.run([str(RENDER_BIN)], input=svg_bytes, env=env,
                           capture_output=True, timeout=30)
        return r.returncode == 0 and output_path.exists()
    except subprocess.TimeoutExpired:
        print(f'  loader render timed out: {svg_path.name}', file=sys.stderr)
        return False


# Tools for the transpiled column (built on demand).
TRANSPILE_MODULE_BIN = AEVG_ROOT / 'build' / 'svg_transpile_module'   # vg{} source (for display)
TRANSPILE_HARNESS_BIN = AEVG_ROOT / 'build' / 'svg_transpile_harness' # PNG harness emitter


def _ensure_built(src_rel: str, bin_path: Path) -> bool:
    """Build an .ae tool if its binary is missing. Returns True if present."""
    if bin_path.exists():
        return True
    src = AEVG_ROOT / src_rel
    if not src.exists():
        return False
    r = subprocess.run([str(BUILD_SH), src_rel, f'build/{bin_path.name}'],
                       cwd=str(AEVG_ROOT), capture_output=True, text=True)
    return bin_path.exists()


def transpile_to_source(svg_path: Path) -> str:
    """Emit the human-facing vg{} module source for display (the 'show source'
    pane). Best-effort; '' if the emitter tool isn't available."""
    if not _ensure_built('aevg/svg_transpile_module.ae', TRANSPILE_MODULE_BIN):
        return ''
    try:
        r = subprocess.run([str(TRANSPILE_MODULE_BIN)],
                           input=svg_path.read_bytes(),
                           capture_output=True, timeout=20)
        return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else ''
    except Exception:
        return ''


def render_transpiled(svg_path: Path, output_path: Path, work: Path) -> bool:
    """Transpile the SVG to a headless PNG-render harness, BUILD it, RUN it →
    PNG. This proves the generated vg{} source compiles and renders. Returns
    True on success. Each SVG gets its own throwaway module + binary under
    aevg/ (build.sh resolves imports relative to the source dir, so the .ae
    must live there)."""
    if not _ensure_built('aevg/svg_transpile_harness.ae', TRANSPILE_HARNESS_BIN):
        return False
    stem = svg_path.stem
    # 1. Emit the harness .ae for this SVG.
    try:
        r = subprocess.run([str(TRANSPILE_HARNESS_BIN)],
                           input=svg_path.read_bytes(),
                           capture_output=True, timeout=20)
        if r.returncode != 0 or not r.stdout:
            return False
    except Exception:
        return False
    # The generated module must sit in aevg/ for import resolution.
    safe = ''.join(c if (c.isalnum() or c == '_') else '_' for c in stem)
    gen_ae = AEVG_ROOT / 'aevg' / f'_gen_{safe}.ae'
    gen_bin = AEVG_ROOT / 'build' / f'_gen_{safe}'
    try:
        gen_ae.write_bytes(r.stdout)
        # 2. Build it.
        b = subprocess.run([str(BUILD_SH), f'aevg/_gen_{safe}.ae', f'build/_gen_{safe}'],
                           cwd=str(AEVG_ROOT), capture_output=True, text=True, timeout=120)
        if not gen_bin.exists():
            return False
        # 3. Run it headless → PNG.
        env = os.environ.copy()
        env['AETHER_UI_HEADLESS'] = '1'
        env['AEVG_OUT'] = str(output_path)
        env['AEVG_SIZE'] = str(SIZE)
        rr = subprocess.run([str(gen_bin)], env=env, capture_output=True, timeout=30)
        return rr.returncode == 0 and output_path.exists()
    except subprocess.TimeoutExpired:
        return False
    finally:
        gen_ae.unlink(missing_ok=True)
        gen_bin.unlink(missing_ok=True)


def png_to_data_uri(png_path: Path) -> str:
    data = png_path.read_bytes()
    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')


def pixel_mae(ref_path: Path, other_white: Image.Image) -> float:
    import numpy as np
    ref = np.array(Image.open(ref_path).convert('RGB'), dtype=float)
    oth = other_white
    if oth.size != (ref.shape[1], ref.shape[0]):
        oth = oth.resize((ref.shape[1], ref.shape[0]))
    o = np.array(oth, dtype=float)
    return float(np.mean(np.abs(ref - o)))


def bucket(mae: float) -> str:
    if mae < 0:
        return 'none'
    if mae < 20:
        return 'good'
    if mae < 40:
        return 'ok'
    return 'diff'


def generate_html(results: list[dict], html_path: Path, has_transpile: bool):
    good = sum(1 for r in results if 0 <= r['loader_mae'] < 20)
    ok = sum(1 for r in results if 20 <= r['loader_mae'] < 40)
    bad = sum(1 for r in results if r['loader_mae'] >= 40)
    none = sum(1 for r in results if r['loader_mae'] < 0)

    rows = []
    for r in results:
        m = r['loader_mae']
        b = bucket(m)
        badge = {'good': 'GOOD', 'ok': 'OK', 'diff': 'DIFF', 'none': '?'}[b]
        mae_str = f'MAE {m:.1f}' if m >= 0 else 'no render'

        def panel(label, uri, cls):
            if uri:
                img = f'<img src="{uri}" width="300" height="300">'
            else:
                img = '<div class="no-shot">—</div>'
            return f'<div class="panel"><div class="label {cls}">{label}</div><div class="wrap">{img}</div></div>'

        cols = [
            panel('Reference (librsvg)', r['ref_uri'], 'ref'),
            panel(f'Loader · {mae_str}', r.get('loader_uri'), 'loader'),
        ]
        if has_transpile:
            tm = r['transpiled_mae']
            tlabel = f'Transpiled · MAE {tm:.1f}' if tm >= 0 else 'Transpiled · —'
            cols.append(panel(tlabel, r.get('transpiled_uri'), 'transpiled'))

        src = html_mod.escape(r.get('svg_source', ''))
        tsrc = html_mod.escape(r.get('transpiled_source', ''))
        src_links = '<a href="#" class="src" onclick="tog(this,\'svg\');return false">svg</a>'
        src_blocks = f'<div class="srcpane" data-k="svg"><pre><code class="language-xml">{src}</code></pre></div>'
        if tsrc:
            src_links += ' <a href="#" class="src" onclick="tog(this,\'ae\');return false">vg{} source</a>'
            src_blocks += f'<div class="srcpane" data-k="ae"><pre><code>{tsrc}</code></pre></div>'

        rows.append(f'''
    <div class="cmp" data-bucket="{b}">
      <div class="hdr"><span class="name">{r['name']}</span>
        <span class="badge {b}">{badge}</span>
        <span class="links">{src_links}</span></div>
      <div class="imgs">{''.join(cols)}</div>
      {src_blocks}
    </div>''')

    html = f'''<!DOCTYPE html><html><head><meta charset="utf-8">
<title>AeVG SVG Conformance</title>
<style>
 body{{font-family:system-ui,sans-serif;margin:0;background:#f5f5f5}}
 .sticky{{position:sticky;top:0;background:#f5f5f5;padding:16px 20px 8px;border-bottom:1px solid #ddd;z-index:5}}
 h1{{margin:0 0 4px;font-size:20px}} .sum{{color:#666;font-size:14px}}
 .sum .g{{color:#2a2}} .sum .o{{color:#a80}} .sum .d{{color:#c22}}
 .filters{{margin-top:8px;display:flex;gap:6px}}
 .filters input{{padding:6px 10px;border:1px solid #ccc;border-radius:4px;width:200px}}
 .filters button{{padding:6px 12px;border:1px solid #ccc;border-radius:4px;background:#fff;cursor:pointer}}
 .filters button.on{{background:#333;color:#fff}}
 #list{{padding:16px 20px}}
 .cmp{{background:#fff;border-radius:8px;margin-bottom:14px;padding:14px;box-shadow:0 1px 3px rgba(0,0,0,.1)}}
 .hdr{{display:flex;align-items:center;gap:10px;margin-bottom:10px}}
 .name{{font-weight:600}} .links{{margin-left:auto;font-size:12px}}
 .badge{{padding:2px 8px;border-radius:3px;font-size:11px;font-weight:700}}
 .badge.good{{background:#d4edda;color:#155724}} .badge.ok{{background:#fff3cd;color:#856404}}
 .badge.diff{{background:#f8d7da;color:#721c24}} .badge.none{{background:#e2e3e5;color:#383d41}}
 .imgs{{display:flex;gap:14px;flex-wrap:wrap}}
 .panel{{text-align:center}} .panel img,.no-shot{{width:300px;height:300px;border:1px solid #ddd;background:#fff}}
 .no-shot{{display:flex;align-items:center;justify-content:center;color:#bbb;font-size:24px}}
 .label{{font-size:12px;color:#666;margin-bottom:4px}}
 .ref{{color:#2a2}} .loader{{color:#07a}} .transpiled{{color:#a2a}}
 .src{{color:#888;text-decoration:none;margin-left:6px}} .src:hover{{color:#333;text-decoration:underline}}
 .srcpane{{display:none;text-align:left;background:#282c34;color:#ddd;border-radius:6px;padding:12px;margin-top:10px;max-height:420px;overflow:auto}}
 .srcpane pre{{margin:0}} .srcpane code{{font-family:Menlo,Consolas,monospace;font-size:12px;line-height:1.5}}
</style></head><body>
<div class="sticky">
 <h1>AeVG SVG Conformance</h1>
 <p class="sum">{len(results)} SVGs · <span class="g">{good} good</span>, <span class="o">{ok} ok</span>, <span class="d">{bad} diff</span>, {none} none · loader vs librsvg · MAE 0–255 · worst first</p>
 <div class="filters">
  <input id="q" placeholder="filter by name…" oninput="flt()">
  <button class="on" data-f="all" onclick="setf(this)">All</button>
  <button data-f="diff" onclick="setf(this)">Diff</button>
  <button data-f="ok" onclick="setf(this)">OK</button>
  <button data-f="good" onclick="setf(this)">Good</button>
 </div>
</div>
<div id="list">{''.join(rows)}</div>
<script>
var cf='all';
function setf(b){{cf=b.dataset.f;document.querySelectorAll('.filters button').forEach(x=>x.classList.remove('on'));b.classList.add('on');flt();}}
function flt(){{var q=document.getElementById('q').value.toLowerCase();
 document.querySelectorAll('.cmp').forEach(function(e){{
  var n=e.querySelector('.name').textContent.toLowerCase();
  var ok=(cf=='all'||e.dataset.bucket==cf)&&(!q||n.includes(q));
  e.style.display=ok?'':'none';}});}}
function tog(a,k){{var c=a.closest('.cmp');var p=c.querySelector('.srcpane[data-k="'+k+'"]');
 var open=p.style.display=='block';c.querySelectorAll('.srcpane').forEach(x=>x.style.display='none');
 p.style.display=open?'none':'block';}}
</script></body></html>'''
    html_path.write_text(html)


def main():
    ap = argparse.ArgumentParser(description='AeVG SVG conformance vs librsvg')
    ap.add_argument('files', nargs='*', help='specific SVGs (default: all)')
    ap.add_argument('--svg-dir', type=Path, default=DEFAULT_SVG_DIR)
    ap.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    ap.add_argument('--limit', type=int, default=0, help='only first N (smoke)')
    ap.add_argument('--transpile', action='store_true', help='also build+render transpiled column')
    ap.add_argument('--no-open', action='store_true')
    args = ap.parse_args()

    if not RENDER_BIN.exists():
        print(f'Render binary missing: {RENDER_BIN}\n'
              f'Build it:  ./build.sh aevg/svg_render_png.ae build/svg_render', file=sys.stderr)
        return 1

    svg_dir = args.svg_dir
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    if args.files:
        svgs = []
        for f in args.files:
            p = Path(f)
            if not p.exists():
                p = svg_dir / (f if f.endswith('.svg') else f + '.svg')
            svgs.append(p)
    else:
        svgs = sorted(svg_dir.glob('*.svg'))
    if args.limit > 0:
        svgs = svgs[:args.limit]
    if not svgs:
        print('No SVGs found'); return 1

    results = []
    for i, svg in enumerate(svgs):
        name = svg.name
        print(f'[{i+1}/{len(svgs)}] {name}', end=' ', flush=True)
        ref = out / f'{svg.stem}_ref.png'
        try:
            render_reference(svg, ref)
        except Exception as e:
            print(f'ref FAIL: {e}'); continue

        loader_uri = None
        loader_mae = -1.0
        lpng = out / f'{svg.stem}_loader.png'
        if render_loader(svg, lpng):
            loader_uri = png_to_data_uri(lpng)
            try:
                loader_mae = pixel_mae(ref, _composite_white(lpng))
            except ImportError:
                pass

        transpiled_uri = None
        transpiled_mae = -1.0
        transpiled_source = transpile_to_source(svg) if args.transpile else ''
        if args.transpile:
            tpng = out / f'{svg.stem}_transpiled.png'
            if render_transpiled(svg, tpng, out):
                transpiled_uri = png_to_data_uri(tpng)
                try:
                    transpiled_mae = pixel_mae(ref, _composite_white(tpng))
                except ImportError:
                    pass

        tag = ('?' if loader_mae < 0 else '✓' if loader_mae < 20
               else '~' if loader_mae < 40 else '✗')
        print(f'{tag} {loader_mae:.1f}' if loader_mae >= 0 else '? no-render')

        results.append({
            'name': name,
            'loader_mae': loader_mae,
            'transpiled_mae': transpiled_mae,
            'ref_uri': png_to_data_uri(ref),
            'loader_uri': loader_uri,
            'transpiled_uri': transpiled_uri,
            'svg_source': svg.read_text(encoding='utf-8', errors='replace'),
            'transpiled_source': transpiled_source,
        })

    results.sort(key=lambda r: r['loader_mae'] if r['loader_mae'] >= 0 else 999, reverse=True)

    html_path = out / 'comparison.html'
    generate_html(results, html_path, has_transpile=args.transpile)

    csv_path = out / 'results.csv'
    with open(csv_path, 'w') as f:
        f.write('file,loader_mae,loader_status,transpiled_mae\n')
        for r in results:
            f.write(f'{r["name"]},{r["loader_mae"]:.1f},{bucket(r["loader_mae"])},{r["transpiled_mae"]:.1f}\n')

    # Checked-in conformance snapshot (alpha-sorted) for full runs.
    if not args.files and args.limit == 0:
        conf = SCRIPT_DIR / 'svg-conformance-aevg.csv'
        with open(conf, 'w') as f:
            f.write('file,loader_mae,status\n')
            for r in sorted(results, key=lambda r: r['name'].lower()):
                f.write(f'{r["name"]},{r["loader_mae"]:.1f},{bucket(r["loader_mae"])}\n')
        print(f'Conformance snapshot: {conf}')

    good = sum(1 for r in results if 0 <= r['loader_mae'] < 20)
    okc = sum(1 for r in results if 20 <= r['loader_mae'] < 40)
    bad = sum(1 for r in results if r['loader_mae'] >= 40)
    none = sum(1 for r in results if r['loader_mae'] < 0)
    print(f'\n{"="*46}')
    print(f'{len(results)} SVGs · {good} good · {okc} ok · {bad} diff · {none} none')
    print(f'HTML: {html_path}')
    print(f'CSV:  {csv_path}')

    if not args.no_open:
        os.system(f'xdg-open "{html_path}" 2>/dev/null &')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
