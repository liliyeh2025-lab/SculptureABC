# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
"""Runs every upstream unit test (Node.js build from test-node.ps1) in its own process, several at once, each with
a time limit, so one test that hangs or crashes the WebAssembly module cannot stop the rest.

    python mobile/tools/test-node-each.py [--jobs 12] [--timeout 600] [name-substring ...]

Logs: build/mobile-node/each/<test>.log; summary: build/mobile-node/each-summary.txt (also printed).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BUILD = os.path.join(ROOT, 'build', 'mobile-node')


def test_names():
    names = []
    for f in sorted(os.listdir(os.path.join(ROOT, 'tests'))):
        if f.startswith('test_') and f.endswith('.cpp'):
            with open(os.path.join(ROOT, 'tests', f), encoding='utf-8') as fh:
                names += re.findall(r'^TEST\((\w+)\)', fh.read(), re.M)
    return names


def run(name, timeout):
    log = os.path.join(BUILD, 'each', name + '.log')
    start = time.time()
    try:
        p = subprocess.run(['node', '--stack-size=8000', 'sculptforge_tests.js', name], cwd=BUILD,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        out = p.stdout.decode('utf-8', 'replace')
        timed_out = False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode('utf-8', 'replace')
        timed_out = True
    with open(log, 'w', encoding='utf-8') as fh:
        fh.write(out)
    secs = time.time() - start
    if timed_out:
        return name, 'TIMEOUT', secs, ''
    if re.search(r'^\[ OK \] %s \(' % re.escape(name), out, re.M):
        return name, 'OK', secs, ''
    first = ''
    for line in out.splitlines():
        if line.startswith('  FAILED') or line.startswith('  EXCEPTION') or 'Aborted' in line or 'RuntimeError' in line:
            first = line.strip()
            break
    status = 'FAIL' if re.search(r'^\[FAIL\] %s \(' % re.escape(name), out, re.M) else 'CRASH'
    if status == 'CRASH' and not first:
        first = (out.strip().splitlines() or [''])[-1][:200]
    return name, status, secs, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('filters', nargs='*')
    a = ap.parse_args()
    os.makedirs(os.path.join(BUILD, 'each'), exist_ok=True)
    names = [n for n in test_names() if not a.filters or any(f in n for f in a.filters)]
    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda n: run(n, a.timeout), names):
            results.append(r)
    counts = {}
    for _, s, _, _ in results:
        counts[s] = counts.get(s, 0) + 1
    lines = ['%d tests: ' % len(results) + ', '.join('%s %d' % kv for kv in sorted(counts.items()))]
    for n, s, secs, first in results:
        if s != 'OK':
            lines.append('%-8s %-60s %6.0fs  %s' % (s, n, secs, first))
    slow = sorted((r for r in results if r[1] == 'OK'), key=lambda r: -r[2])[:10]
    lines.append('slowest passing: ' + ', '.join('%s %.0fs' % (n, secs) for n, _, secs, _ in slow))
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(BUILD, 'each-summary.txt'), 'w', encoding='utf-8') as fh:
        fh.write(text)
    print(text)


if __name__ == '__main__':
    main()
