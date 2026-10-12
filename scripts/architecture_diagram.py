"""Draws docs/architecture.svg from the #include graph in src/.

Every module (a .hpp/.cpp pair) is a UML component. Modules are layered by
dependency stability: third-party libraries on L0, then each module one level
below the lowest module it uses, and entry points nothing includes on the bottom
row. Every arrow points up. Arrows are the transitive reduction of the graph.

Usage: python scripts/architecture_diagram.py [output.svg]
Needs networkx and git on PATH.
"""
import collections, pathlib, re, subprocess, sys
import networkx as nx

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'docs' / 'architecture.svg'

EXT_HEADERS = {'windows.h': 'Win32', 'bcrypt.h': 'Win32', 'wincodec.h': 'Win32', 'lz4.h': 'LZ4',
               'zstd.h': 'Zstandard', 'sqlite3.h': 'SQLite', 'nlohmann/json.hpp': 'nlohmann::json',
               'meshoptimizer.h': 'meshoptimizer', 'vulkan/vulkan.h': 'Vulkan', 'imgui.h': 'Dear ImGui',
               'tracy/Tracy.hpp': 'Tracy'}


def module_of(path):
    """src/core/vpk.cpp -> vpk; the two main.cpp files keep their directory."""
    p = pathlib.PurePosixPath(path)
    return f'{p.parent.name}/main' if p.stem == 'main' else p.stem


# ---------- graph ----------
pkg, dep = {}, collections.defaultdict(set)
for f in sorted((ROOT / 'src').glob('*/*.[ch]pp')):
    n = module_of(f.as_posix()); pkg[n] = f.parent.name
    for inc in re.findall(r'#include\s*([<"])([^>"]+)[>"]', f.read_text(encoding='utf-8', errors='replace')):
        kind, name = inc
        if kind == '<':
            if name in EXT_HEADERS: dep[n].add(EXT_HEADERS[name])
        elif not name.endswith('.inc'):
            d = pathlib.PurePosixPath(name).stem
            if d != n: dep[n].add(d)
for e in set(EXT_HEADERS.values()): pkg[e] = 'ext'
for n in dep: dep[n] &= set(pkg)  # drop includes that are not project modules

churn = collections.Counter()
log = subprocess.run(['git', 'log', '--name-only', '--pretty=format:@', '--', 'src'], cwd=ROOT,
                     capture_output=True, text=True, check=True).stdout
for commit in log.split('@'):
    for m in {module_of(p) for p in commit.split() if re.search(r'\.[ch]pp$', p)}: churn[m] += 1

G = nx.DiGraph()
for n in pkg: G.add_node(n)
for a, ds in dep.items():
    for d in ds: G.add_edge(a, d)
assert nx.is_directed_acyclic_graph(G)
ca = {n: G.in_degree(n) for n in G}
ce = {n: sum(1 for d in G.successors(n) if pkg[d] != 'ext') for n in G}
inst = {n: (ce[n] / (ca[n] + ce[n]) if ca[n] + ce[n] else 0.0) for n in G}
R = nx.transitive_reduction(G)
REAL = ('preview/main', 'model_tools')

# ---------- layering: longest path to a sink; externals pinned to level 0 ----------
level = {}
for n in reversed(list(nx.topological_sort(G))):
    if pkg[n] == 'ext': level[n] = 0
    else: level[n] = 1 + max([level[d] for d in G.successors(n) if pkg[d] != 'ext'] + [0])
NL = max(level.values()) + 1
for n in G:  # entry points nobody includes are the most volatile: pin them to the bottom row
    if pkg[n] != 'ext' and G.in_degree(n) == 0: level[n] = NL - 1

# ---------- dummy nodes for long edges ----------
layers = [[] for _ in range(NL)]
for n in G: layers[level[n]].append(n)
chains, adj_up, adj_dn = {}, collections.defaultdict(list), collections.defaultdict(list)
k = 0
for a, b in R.edges():
    path = [a]
    for L in range(level[a] - 1, level[b], -1):
        dn = f'~{k}'; k += 1; layers[L].append(dn); level[dn] = L; path.append(dn)
    path.append(b); chains[(a, b)] = path
    for u, v in zip(path, path[1:]):
        adj_up[u].append(v); adj_dn[v].append(u)

ORDER_PKG = {'ext': 0, 'core': 1, 'render': 2, 'preview': 3, 'app': 4}
for L in layers: L.sort(key=lambda n: (ORDER_PKG.get(pkg.get(n), 9), n))
pos = {n: i for L in layers for i, n in enumerate(L)}


def crossings():
    c = 0
    for L in range(1, NL):
        es = [(pos[u], pos[v]) for u in layers[L] for v in adj_up[u]]
        for i in range(len(es)):
            for j in range(i + 1, len(es)):
                if (es[i][0] - es[j][0]) * (es[i][1] - es[j][1]) < 0: c += 1
    return c


def sweep(down):
    rng = range(1, NL) if down else range(NL - 2, -1, -1)
    nb = adj_up if down else adj_dn
    for L in rng:
        def bc(n):
            ns = nb[n]
            return sum(pos[m] for m in ns) / len(ns) if ns else pos[n]
        layers[L].sort(key=bc)
        for i, n in enumerate(layers[L]): pos[n] = i


best, bestc = None, 10 ** 9
for _ in range(40):
    sweep(True); sweep(False)
    c = crossings()
    if c < bestc: bestc, best = c, [list(L) for L in layers]
layers = best
pos = {n: i for L in layers for i, n in enumerate(L)}

# ---------- coordinates: compact rows, slid toward their neighbours ----------
BW, BH, GAP = 176, 60, 40
LH = 150; TOP = 170; LEFT = 110
rows = [[n for n in L if not n.startswith('~')] for L in layers]
nbr = collections.defaultdict(list)
for a_, b_ in R.edges(): nbr[a_].append(b_); nbr[b_].append(a_)
MAXW = max(len(r) for r in rows) * (BW + GAP) - GAP
x = {}
for r in rows:
    off = (MAXW - (len(r) * (BW + GAP) - GAP)) / 2
    for i, n in enumerate(r): x[n] = off + i * (BW + GAP) + BW / 2
for it in range(40):
    for r in rows:
        want = {n: (sum(x[m] for m in nbr[n]) / len(nbr[n]) if nbr[n] else x[n]) for n in r}
        r.sort(key=lambda n: want[n])
        span = len(r) * (BW + GAP) - GAP
        mean = sum(want.values()) / len(r)
        off = min(max(mean - span / 2, 0), MAXW - span)
        base = [off + i * (BW + GAP) + BW / 2 for i in range(len(r))]
        # let each box drift toward its wish without breaking order or spacing
        xs = [0.5 * bx + 0.5 * want[n] for bx, n in zip(base, r)]
        for i in range(1, len(xs)): xs[i] = max(xs[i], xs[i - 1] + BW + GAP)
        for i in range(len(xs) - 2, -1, -1): xs[i] = min(xs[i], xs[i + 1] - BW - GAP)
        lo, hi = xs[0] - BW / 2, xs[-1] + BW / 2
        sh = -lo if lo < 0 else (MAXW - hi if hi > MAXW else 0)
        for n, v in zip(r, xs): x[n] = v + sh
for n in x: x[n] += LEFT
W = int(MAXW + LEFT + 60)
y = {n: TOP + level[n] * LH for n in G}
H = TOP + (NL - 1) * LH + BH + 80

# ports: spread arrow ends across the box edge in the order of the far endpoint
outp, inp = {}, {}
for n in G:
    ups = sorted(R.successors(n), key=lambda m: x[m])
    for i, m in enumerate(ups): outp[(n, m)] = x[n] + (i + 1) / (len(ups) + 1) * BW * 0.8 - BW * 0.4
    dns = sorted(R.predecessors(n), key=lambda m: x[m])
    for i, m in enumerate(dns): inp[(m, n)] = x[n] + (i + 1) / (len(dns) + 1) * BW * 0.8 - BW * 0.4

# ---------- svg ----------
FILL = {'ext': ('#eef0f3', '#59636e'), 'core': ('#e7eef5', '#3d5a73'), 'render': ('#f3eef8', '#5b4580'),
        'preview': ('#fff4e0', '#9a6416'), 'app': ('#e7f3ec', '#2c6b4a')}
LABEL = {'ext': 'Third-party and OS', 'core': 'skeleton_repair_core', 'render': 'sr_render',
         'preview': 'sr_model_tools', 'app': 'DeadlockSkeletonRepair.exe'}
o = []; w = o.append
w(f'<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img">')
w('<title>Deadlock Mod Manager dependency levels</title>')
w('<desc>UML component diagram of the #include graph in src, layered by stability. Stable modules are at the top, volatile modules at the bottom, and every dependency arrow points up.</desc>')
w('<defs>'
  '<marker id="use" viewBox="0 0 12 12" refX="11" refY="6" markerWidth="10" markerHeight="10" orient="auto"><path d="M1,1 L11,6 L1,11" fill="none" stroke="#5f6b78" stroke-width="1.5"/></marker>'
  '<marker id="real" viewBox="0 0 14 14" refX="13" refY="7" markerWidth="13" markerHeight="13" orient="auto"><path d="M1,1 L13,7 L1,13 Z" fill="#ffffff" stroke="#2c6b4a" stroke-width="1.4"/></marker>'
  '</defs>')
w("<style>text{font-family:'Segoe UI','Helvetica Neue',Arial,sans-serif} .u{fill:none;stroke:#7b8794;stroke-width:1.15;stroke-dasharray:5 3}</style>")
w(f'<rect width="{W}" height="{H}" fill="#fbfaf7"/>')
w('<text x="36" y="46" fill="#1c2430" font-size="26" font-weight="650">Deadlock Mod Manager: dependency levels</text>')
w('<text x="36" y="72" fill="#526272" font-size="14">Every #include in src/, drawn as UML components. A module sits one level below the lowest module it uses, so every arrow points up and nothing depends on a module beneath it.</text>')
w('<text x="36" y="92" fill="#526272" font-size="14">Arrows are the transitive reduction: an arrow already implied by a longer path is left out. I = Ce / (Ca + Ce). Ca and Ce count project modules only. Commits are changes to the file pair in git history.</text>')
lx = 36
for p in ['ext', 'core', 'render', 'preview', 'app']:
    f, s = FILL[p]
    w(f'<rect x="{lx}" y="112" width="14" height="14" rx="2" fill="{f}" stroke="{s}" stroke-width="1.2"/><text x="{lx + 20}" y="124" fill="#3e4c5c" font-size="12.5">{LABEL[p]}</text>')
    lx += 34 + len(LABEL[p]) * 7
w(f'<path d="M{lx + 10},119 H{lx + 52}" class="u" marker-end="url(#use)"/><text x="{lx + 58}" y="124" fill="#3e4c5c" font-size="12.5">«use» (#include)</text>')
lx += 180
w(f'<path d="M{lx},119 H{lx + 42}" fill="none" stroke="#2c6b4a" stroke-width="1.4" stroke-dasharray="6 3" marker-end="url(#real)"/><text x="{lx + 48}" y="124" fill="#3e4c5c" font-size="12.5">realizes</text>')

for L in range(NL):
    yy = TOP + L * LH
    w(f'<rect x="20" y="{yy - (LH - BH) / 2 + 4:.0f}" width="{W - 40}" height="{LH - 8}" rx="8" fill="{"#f3f1ec" if L % 2 == 0 else "#fbfaf7"}"/>')
    w(f'<text x="34" y="{yy + BH / 2 - 2}" fill="#6b7685" font-size="14" font-weight="650">L{L}</text>')
w(f'<text x="34" y="{TOP + BH / 2 + 16}" fill="#8792a0" font-size="10.5">most</text><text x="34" y="{TOP + BH / 2 + 29}" fill="#8792a0" font-size="10.5">stable</text>')
yl = TOP + (NL - 1) * LH
w(f'<text x="34" y="{yl + BH / 2 + 16}" fill="#8792a0" font-size="10.5">most</text><text x="34" y="{yl + BH / 2 + 29}" fill="#8792a0" font-size="10.5">volatile</text>')

for (a, b) in R.edges():
    x0, y0, x1, y1 = outp[(a, b)], y[a], inp[(a, b)], y[b] + BH
    k_ = min(LH * 0.55, (y0 - y1) / 2)
    d = f'M{x0:.1f},{y0} C{x0:.1f},{y0 - k_:.1f} {x1:.1f},{y1 + k_:.1f} {x1:.1f},{y1}'
    if (a, b) == REAL:
        w(f'<path d="{d}" fill="none" stroke="#2c6b4a" stroke-width="1.6" stroke-dasharray="6 3" marker-end="url(#real)"/>')
    else:
        w(f'<path d="{d}" class="u" marker-end="url(#use)"><title>{a} uses {b}</title></path>')


def esc(s): return s.replace('&', '&amp;').replace('<', '&lt;')


for n in G:
    p = pkg[n]; f, s = FILL[p]; x0 = x[n] - BW / 2; y0 = y[n]
    w(f'<g><title>{esc(n)}</title><rect x="{x0:.1f}" y="{y0}" width="{BW}" height="{BH}" rx="3" fill="{f}" stroke="{s}" stroke-width="1.3"/>')
    stereo = '«library»' if p == 'ext' else ('«interface»' if n == 'model_tools' else '«component»')
    if p == 'ext':
        w(f'<text x="{x[n]:.1f}" y="{y0 + 22}" fill="{s}" font-size="10.5" text-anchor="middle" font-style="italic">{stereo}</text>')
        w(f'<text x="{x[n]:.1f}" y="{y0 + 41}" fill="#1c2430" font-size="14" font-weight="650" text-anchor="middle">{esc(n)}</text>')
    else:
        w(f'<text x="{x[n]:.1f}" y="{y0 + 15}" fill="{s}" font-size="10.5" text-anchor="middle" font-style="italic">{stereo}</text>')
        w(f'<text x="{x[n]:.1f}" y="{y0 + 32}" fill="#1c2430" font-size="13.5" font-weight="650" text-anchor="middle">{esc(n)}</text>')
        w(f'<line x1="{x0:.1f}" y1="{y0 + 39}" x2="{x0 + BW:.1f}" y2="{y0 + 39}" stroke="{s}" stroke-width=".8" opacity=".45"/>')
        c = churn.get(n, 0)
        w(f'<text x="{x[n]:.1f}" y="{y0 + 53}" fill="#526272" font-size="11" text-anchor="middle">I {inst[n]:.2f} · Ca {ca[n]} · Ce {ce[n]} · {c} commit{"s" if c != 1 else ""}</text>')
    w('</g>')
w('</svg>')
open(OUT, 'w', encoding='utf-8').write('\n'.join(o) + '\n')
print(f'{OUT}: {len(G)} modules on {NL} levels, {R.number_of_edges()} arrows')
