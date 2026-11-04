#!/usr/bin/env python3
"""Compare factorized MR binaries on medium/hard inputs in fresh processes.

Uses the platform's /usr/bin/time for whole-process peak RSS. Candidate stage
high-water marks are kept separately; they are not stage-local peak RSS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile


DEFAULT_CASES = ['imagick', 'x264', 'cactus', 'omnetpp', 'scipiex', 'simhosy', 'phospy']
VALUE_FLOW = {'imagick', 'x264', 'cactus', 'omnetpp', 'perlbench', 'xz',
              'leela', 'nab', 'parest', 'povray'}


def evaluate(binary, graph, analysis, factorized, stage_stats, timeout,
             method='mutual-refinement'):
    command = [str(binary), 'staged-bounds', '--analysis', analysis,
               '--method', method, '--print-result']
    if factorized:
        command.append('--factorized-tracing')
    if stage_stats:
        command.append('--stage-stats')
    command.append(str(graph))
    darwin = sys.platform == 'darwin'
    timer = ['/usr/bin/time', '-l' if darwin else '-v']
    with tempfile.TemporaryDirectory(prefix='lotus-staged-') as temporary:
        directory = Path(temporary)
        stdout = directory / 'stdout'
        stderr = directory / 'stderr'
        with stdout.open('w') as out, stderr.open('w') as err:
            process = subprocess.Popen(timer + command, stdout=out, stderr=err,
                                       start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                return {'status': 'timeout', 'timeout_s': timeout,
                        'diagnostics': stderr.read_text()}
        diagnostics = stderr.read_text()
        if code:
            return {'status': 'error', 'returncode': code,
                    'diagnostics': diagnostics}
        pattern = (r'(\d+)\s+maximum resident set size' if darwin else
                   r'Maximum resident set size \(kbytes\):\s*(\d+)')
        match = re.search(pattern, diagnostics)
        if not match:
            return {'status': 'error', 'diagnostics': diagnostics,
                    'error': 'timer did not report peak RSS'}
        rss_mib = int(match[1]) / (1024 * 1024 if darwin else 1024)
        pairs = directory / 'pairs'
        count = 0
        solver_ms = None
        with stdout.open() as output, pairs.open('w') as pair_output:
            for line in output:
                if line.startswith('Time (ms):'):
                    solver_ms = int(line.split(':')[1])
                elif re.fullmatch(r'-?\d+ -?\d+\n?', line):
                    pair_output.write(line)
                    count += 1
        # Sort outside the measured process, with bounded-memory external sort.
        # Hash all pairs, not just their count: their iteration order is unstable.
        ordered = directory / 'ordered'
        subprocess.run(['sort', '-o', str(ordered), str(pairs)], check=True,
                       env={**os.environ, 'LC_ALL': 'C'})
        digest = hashlib.sha256()
        with ordered.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        return {'status': 'ok', 'solver_ms': solver_ms, 'peak_rss_mib': rss_mib,
                'pairs': count, 'pairs_sha256': digest.hexdigest(),
                'stages': [line for line in diagnostics.splitlines()
                           if line.startswith('Stage ')]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--dataset', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'benchmarks/real-world/CFL/InterleavedDyck')
    parser.add_argument('--benchmarks', nargs='+', default=DEFAULT_CASES)
    parser.add_argument('--method', choices=['mutual-refinement', 'stronger-grammar', 'on-demand'],
                        default='mutual-refinement')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--include-eager', action='store_true',
                        help='also measure candidate eager tracing to separate shared engine gains')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.timeout <= 0 or args.repeats <= 0:
        parser.error('timeout and repeats must be positive')
    cases = []
    for name in args.benchmarks:
        analysis = 'value-flow' if name in VALUE_FLOW else 'taint'
        graph = args.dataset / ('valueflow' if analysis == 'value-flow' else 'taint') / (name + '.dot')
        if not graph.is_file():
            parser.error('missing benchmark: ' + str(graph))
        cases.append((name, graph, analysis))
    results = []
    failed = False
    for name, graph, analysis in cases:
        expected = None
        reference_mode = None
        for repeat in range(args.repeats):
            modes = [('baseline-factor', args.baseline, True, False),
                     ('candidate-factor', args.candidate, True, True)]
            if args.include_eager:
                modes.append(('candidate-eager', args.candidate, False, True))
            for mode, binary, factorized, stats in modes:
                result = evaluate(binary.resolve(), graph.resolve(), analysis,
                                  factorized, stats, args.timeout, args.method)
                result.update(benchmark=name, mode=mode, repeat=repeat, method=args.method)
                if result['status'] == 'ok':
                    signature = (result['pairs'], result['pairs_sha256'])
                    if expected is None:
                        result['matches_other_completed_runs'] = None
                        result['comparison_reference_mode'] = None
                        expected = signature
                        reference_mode = mode
                    else:
                        result['matches_other_completed_runs'] = signature == expected
                        result['comparison_reference_mode'] = reference_mode
                        failed |= signature != expected
                failed |= result['status'] == 'error'
                results.append(result)
                args.output.write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(result), flush=True)
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
