#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# dev/bench-ec2.d/report.py RESULTS_DIR: the PLAN section 9 table from a
# dev/bench-ec2 run, as Markdown.  Repetition 0 of each point is the
# warm-up and is discarded; every figure is the median of the rest, with
# the min-max spread of throughput.
import csv
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path


def quantiles(path):
    """p50/p99/p99.9 from arrange.rs LATENCY rows: (ns, fraction above)."""
    pts = []
    for line in path.read_text().splitlines():
        f = line.split('\t')
        if len(f) > 3 and f[0] == 'ARRANGE' and f[1] == 'LATENCY':
            pts.append((int(f[-2]), float(f[-1])))
    if not pts:
        return None
    pts.sort()
    q = {}
    for p in (0.5, 0.99, 0.999):
        above = [lat for lat, fa in pts if fa <= 1 - p]
        q[p] = above[0] if above else pts[-1][0]
    return q


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    d = Path(sys.argv[1])
    rows = [r for r in csv.DictReader(open(d / 'runs.csv')) if r['rep'] != '0']
    pts = defaultdict(list)
    for r in rows:
        pts[(r['comp'], r['mode'], int(r['workers']), int(r['rate']), r['impl'])].append(r)
    print((d / 'host.txt').read_text().strip().replace('\n', '  \n'))
    print()
    print('## Throughput (closed loop)\n')
    print('| arm | workers | C rec/s | Rust rec/s | C slower by | instr/rec C/R | futex/rec C/R |')
    print('|---|---|---|---|---|---|---|')
    for comp in ('arrange', 'count'):
        for w in sorted({k[2] for k in pts if k[0] == comp and k[1] == 'closed'}):
            c, r = pts.get((comp, 'closed', w, 1000, 'c'), []), pts.get((comp, 'closed', w, 1000, 'r'), [])
            if not c or not r:
                continue
            def med(rs, f):
                v = [f(x) for x in rs]
                v = [x for x in v if x is not None]
                return st.median(v) if v else None
            tp = lambda x: num(x['recs_per_s'])
            # perf counts the whole process, loading included, so per-record
            # figures are approximate (loading is 100,000 records).
            dur = lambda x: 25.0
            ipr = lambda x: (num(x['instr']) or 0) / dur(x) / tp(x) if tp(x) else None
            fpr = lambda x: (num(x['futex']) or 0) / dur(x) / tp(x) if tp(x) else None
            c = [x for x in c if tp(x)]
            r = [x for x in r if tp(x)]
            if not c or not r:
                print('| %s | %d | - | - | - | - | - |' % (comp, w))
                continue
            ct, rt = med(c, tp), med(r, tp)
            spread = '%.2f-%.2f' % (min(tp(x) for x in c) / 1e6, max(tp(x) for x in c) / 1e6)
            ci, ri = med(c, ipr), med(r, ipr)
            cf, rf = med(c, fpr), med(r, fpr)
            print('| %s | %d | %.2fM [%s] | %.2fM | %.2fx | %s | %s |' % (
                comp, w, ct / 1e6, spread, rt / 1e6, rt / ct,
                '%.2f' % (ci / ri) if ci and ri else '-',
                '%.4f / %.4f' % (cf, rf) if cf is not None and rf is not None else '-'))
    print('\n## Open-loop latency, one worker\n')
    print('| arm | offered rec/s | C p50 / p99 / p99.9 us | Rust p50 / p99 / p99.9 us |')
    print('|---|---|---|---|')
    for comp in ('arrange', 'count'):
        for rate in sorted({k[3] for k in pts if k[0] == comp and k[1] == 'open'}):
            cells = []
            for impl in ('c', 'r'):
                qs = [quantiles(d / ('%s-%s-w1-open-r%d-%s.out' % (impl, comp, rate, x['rep'])))
                      for x in pts.get((comp, 'open', 1, rate, impl), [])]
                qs = [q for q in qs if q]
                if not qs:
                    cells.append('-')
                    continue
                cells.append(' / '.join('%.1f' % (st.median(q[p] for q in qs) / 1e3)
                                        for p in (0.5, 0.99, 0.999)))
            print('| %s | %d | %s | %s |' % (comp, rate, cells[0], cells[1]))
    step_table(d)


def step_table(d):
    path = d / 'step.csv'
    if not path.exists():
        return
    rows = [r for r in csv.DictReader(open(path)) if r['rep'] != '0']
    print('\n## Idle-step cost (barrier rounds)\n')
    print('| workers | C ns/round | Rust ns/round | C slower by |')
    print('|---|---|---|---|')
    for w in sorted({int(r['workers']) for r in rows}):
        ns = {}
        for impl in ('c', 'r'):
            v = [float(r['seconds']) / float(r['rounds']) * 1e9 for r in rows
                 if r['impl'] == impl and int(r['workers']) == w]
            ns[impl] = st.median(v) if v else None
        if ns['c'] and ns['r']:
            print('| %d | %.0f | %.0f | %.2fx |' % (w, ns['c'], ns['r'], ns['c'] / ns['r']))


if __name__ == '__main__':
    main()
