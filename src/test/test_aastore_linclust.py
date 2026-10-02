"""Compare native MMseqs and the minimal AAStore random-access integration."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()

    root = args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary = str(args.mmseqs.resolve())
    base_env = os.environ.copy()
    base_env['LC_ALL'] = 'C'
    runs = []
    checks = []

    def run(label, command):
        started = time.monotonic()
        with (root / (label + '.log')).open('w') as output:
            subprocess.run(
                [str(value) for value in command],
                env=base_env,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=300)
        runs.append({
            'label': label,
            'seconds': time.monotonic() - started,
            'command': [str(value) for value in command],
        })
        (root / 'runs.json').write_text(json.dumps(runs, indent=2))
        print(label + ': passed', flush=True)

    def digest(label, query_db, target_db, result):
        tsv = root / (label + '.tsv')
        run(
            label + '_tsv',
            [binary, 'createtsv', query_db, target_db, result, tsv,
             '-v', '0'])
        rows = sorted(tsv.read_bytes().splitlines(keepends=True))
        return hashlib.sha256(b''.join(rows)).hexdigest()

    database = root / 'native'
    store = root / 'native.aastore'
    rng = random.Random(991)
    alphabet = 'ACDEFGHIKLMNPQRSTVWY'
    sequences = []
    for family in range(12):
        base = ''.join(
            rng.choice(alphabet) for _ in range(240 + family))
        mutated = ''.join(
            residue if position < 14 or position % 3 == 0
            else rng.choice(alphabet)
            for position, residue in enumerate(base))
        sequences.extend((
            base,
            base,
            base[:80] + 'WQWQW' + base[80:],
            mutated,
        ))
    fasta = root / 'fixture.fa'
    fasta.write_text(''.join(
        '>seq{}\n{}\n'.format(index, sequence)
        for index, sequence in enumerate(sequences)))
    run(
        'createdb',
        [binary, 'createdb', fasta, database, '--aastore', '--shuffle', '1',
         '--createdb-mode', '0', '--dbtype', '1', '--threads', '4', '-v', '0'])
    if not all((store / name).is_file() for name in ('packed.bin', 'meta.bin')):
        raise AssertionError('createdb did not build the associated AAStore')

    # Linclust exercises both rescorediagonal passes and, in gapped mode,
    # Alignment. kmermatcher remains the unmodified native implementation.
    for alignment_mode, extra in (
            ('gapped', []),
            ('ungapped', ['--alignment-mode', '4']),
            ('gapped_mode3', ['--cluster-mode', '3'])):
        baseline = None
        cases = [(case, threads) for threads in (1, 4)
                 for case in ('native', 'aastore')]
        for case, threads in cases:
            label = '{}_{}_t{}'.format(alignment_mode, case, threads)
            result = root / label
            run(
                label,
                [binary, 'linclust', database, result,
                 root / (label + '_tmp'), '--threads', str(threads),
                 '--split-memory-limit', '1G', '--min-seq-id', '0.3',
                 '-c', '0.8', '--kmer-per-seq', '80',
                 '--remove-tmp-files', '0', '-v', '3'] + extra +
                ([] if case == 'native' else ['--aastore']))
            value = digest(label, database, database, result)
            if baseline is None:
                baseline = value
            if value != baseline:
                raise AssertionError((label, value, baseline))

            log = (root / (label + '.log')).read_text()
            if alignment_mode == 'gapped_mode3':
                clustering_commands = [
                    shlex.split(line) for line in log.splitlines()
                    if line.startswith('clust ')
                ]
                if len(clustering_commands) != 2:
                    raise AssertionError(label + ' did not run both clustering passes')
                for command in clustering_commands:
                    mode_position = command.index('--cluster-mode') + 1
                    if command[mode_position] != '3':
                        raise AssertionError(label + ' changed the native cluster mode')
            if case != 'native':
                if 'AAStore rescorediagonal:' not in log:
                    raise AssertionError(
                        label + ' did not use AAStore in rescorediagonal')
                if 'io_backend=io_uring' not in log:
                    raise AssertionError(
                        label + ' did not use the default I/O backend')
                if alignment_mode != 'ungapped' and (
                        'AAStore alignment:' not in log or
                        'query=aastore target=aastore' not in log):
                    raise AssertionError(
                        label + ' did not use AAStore in Alignment')
            elif 'AAStore target enabled:' in log:
                raise AssertionError(label + ' enabled AAStore without the flag')
            checks.append({
                'label': label,
                'sha256': value,
                'native_equal': True,
            })

    # An associated subset must still use native reads without --aastore.
    index_rows = [
        tuple(map(int, line.split()))
        for line in Path(str(database) + '.index').read_text().splitlines()
    ]
    stride = max(1, len(index_rows) // 12)
    keys = [row[0] for row in index_rows[::stride]][:12]
    order = root / 'subset_keys'
    order.write_text(''.join(str(key) + '\n' for key in reversed(keys)))
    # createsubdb associates the subset with its parent's store while keeping
    # the ordinary MMseqs data link.
    aastore_subset = root / 'subset_aastore'
    run(
        'createsubdb_aastore',
        [binary, 'createsubdb', order, database, aastore_subset,
         '--subdb-mode', '1', '-v', '0'])
    locator = Path(str(aastore_subset) + '.aastore.path')
    if locator.read_text().strip() != str(store):
        raise AssertionError('createsubdb did not propagate the AAStore path')
    if not aastore_subset.exists():
        raise AssertionError(
            'createsubdb removed the native MMseqs payload')

    # Build a dense all-vs-all candidate list over stable database keys.
    pref = root / 'subset_pref'
    offsets = []
    with pref.open('wb') as output:
        for query in sorted(keys):
            payload = ''.join(
                '{}\t100\t0\n'.format(target)
                for target in keys if target != query).encode() + b'\0'
            offsets.append((query, output.tell(), len(payload)))
            output.write(payload)
    Path(str(pref) + '.index').write_text(''.join(
        '{}\t{}\t{}\n'.format(*entry) for entry in offsets))
    Path(str(pref) + '.dbtype').write_bytes(struct.pack('<I', 7))

    baseline = None
    alignment_cases = [(case, threads) for threads in (1, 4)
                       for case in ('native', 'aastore')]
    for case, threads in alignment_cases:
        label = 'align_subset_{}_t{}'.format(case, threads)
        source = aastore_subset
        result = root / label
        run(
            label,
            [binary, 'align', source, source, pref, result,
             '--threads', str(threads), '--min-seq-id', '0', '-e', '1000000',
             '--alignment-mode', '3', '--realign', '1', '--alt-ali', '1',
             '-a', '1', '-v', '3'] + ([] if case == 'native' else ['--aastore']))
        value = digest(label, source, source, result)
        if baseline is None:
            baseline = value
        if value != baseline:
            raise AssertionError((label, value, baseline))
        log = (root / (label + '.log')).read_text()
        if case != 'native':
            if 'AAStore alignment:' not in log:
                raise AssertionError(
                    label + ' did not use AAStore Alignment')
        elif 'AAStore target enabled:' in log:
            raise AssertionError(label + ' enabled AAStore without the flag')
        checks.append({
            'label': label,
            'sha256': value,
            'native_equal': True,
        })

    (root / 'checks.json').write_text(json.dumps(checks, indent=2))
    print(
        'Minimal AAStore rescorediagonal, Alignment, and createsubdb '
        'checks passed.',
        flush=True)


if __name__ == '__main__':
    main()
