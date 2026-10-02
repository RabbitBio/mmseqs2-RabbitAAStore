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
    parser.add_argument('--builder', required=True, type=Path)
    parser.add_argument('--reorder', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    parser.add_argument('--database', type=Path)
    parser.add_argument('--store', type=Path)
    args = parser.parse_args()

    if bool(args.database) != bool(args.store):
        parser.error('--database and --store must be supplied together')

    root = args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary = str(args.mmseqs.resolve())
    base_env = {
        key: value for key, value in os.environ.items()
        if not key.startswith('MMSEQS_AASTORE')
    }
    base_env['LC_ALL'] = 'C'
    runs = []
    checks = []

    def run(label, command, overrides=None):
        environment = dict(base_env)
        environment.update(overrides or {})
        started = time.monotonic()
        with (root / (label + '.log')).open('w') as output:
            subprocess.run(
                [str(value) for value in command],
                env=environment,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=300)
        runs.append({
            'label': label,
            'seconds': time.monotonic() - started,
            'command': [str(value) for value in command],
            'environment': overrides or {},
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

    if args.database:
        database = args.database.resolve()
        store = args.store.resolve()
    else:
        database = root / 'native'
        store = root / 'store'
        ordered_store = root / 'store_input_order'
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
            [binary, 'createdb', fasta, database, '--shuffle', '1',
             '--createdb-mode', '0', '--dbtype', '1', '-v', '0'])
        run(
            'build_store',
            [args.builder, fasta, ordered_store, '--sequences-only',
             '--threads', '4'])
        run(
            'reorder_store',
            [args.reorder, ordered_store, store, '--threads', '4',
             '--remove-source'])
        if ordered_store.exists():
            raise AssertionError('input-order AAStore was not removed')

    store = store.resolve()

    def aastore_environment(storage, backend='io_uring'):
        values = {
            'MMSEQS_AASTORE': str(store),
            'MMSEQS_AASTORE_IO_BACKEND': backend,
            'MMSEQS_AASTORE_PRELOAD_THREADS': '4',
            'MMSEQS_AASTORE_THREAD_BATCH_BYTES': '512',
            'MMSEQS_AASTORE_FULL_MEMORY': '0',
            'MMSEQS_AASTORE_CACHE_BYTES': '0',
        }
        if storage == 'full':
            values['MMSEQS_AASTORE_FULL_MEMORY'] = '1'
        elif storage == 'prefix':
            values['MMSEQS_AASTORE_CACHE_BYTES'] = '2048'
        return values

    def associated_environment(storage, backend='io_uring'):
        values = aastore_environment(storage, backend)
        del values['MMSEQS_AASTORE']
        return values

    # Linclust exercises both rescorediagonal passes and, in gapped mode,
    # Alignment. kmermatcher remains the unmodified native implementation.
    for alignment_mode, extra in (
            ('gapped', []),
            ('ungapped', ['--alignment-mode', '4']),
            ('gapped_mode3', ['--cluster-mode', '3'])):
        baseline = None
        cases = [
            ('native', {}),
            ('full', aastore_environment('full')),
            ('byte_range', aastore_environment('byte')),
            ('prefix_pread', aastore_environment('prefix', 'pread')),
        ]
        for case, environment in cases:
            if alignment_mode == 'gapped_mode3':
                # The minimal integration must ignore the removed override.
                environment = dict(environment, MMSEQS_FINAL_CLUSTER_MODE='removed')
            label = alignment_mode + '_' + case
            result = root / label
            run(
                label,
                [binary, 'linclust', database, result,
                 root / (label + '_tmp'), '--threads', '4',
                 '--split-memory-limit', '1G', '--min-seq-id', '0.3',
                 '-c', '0.8', '--kmer-per-seq', '80',
                 '--remove-tmp-files', '0', '-v', '3'] + extra,
                environment)
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
                if 'Final cluster mode override:' in log:
                    raise AssertionError(label + ' retained the removed override')

            if case != 'native':
                if 'AAStore rescorediagonal:' not in log:
                    raise AssertionError(
                        label + ' did not use AAStore in rescorediagonal')
                if storage_from_case(case) == 'full':
                    if 'storage_mode=full_memory' not in log:
                        raise AssertionError(
                            label + ' did not use FullMemory')
                else:
                    if 'thread-local batch_get_into' not in log:
                        raise AssertionError(
                            label + ' did not use random batches')
                if case == 'prefix_pread' and (
                        'storage_mode=prefix_memory' not in log or
                        'io_backend=pread' not in log):
                    raise AssertionError(
                        label + ' did not use prefix pread')
                if alignment_mode != 'ungapped' and (
                        'AAStore alignment:' not in log or
                        'query=aastore target=aastore' not in log):
                    raise AssertionError(
                        label + ' did not use AAStore in Alignment')
            checks.append({
                'label': label,
                'sha256': value,
                'native_equal': True,
            })

    # Make one native subset before adding the persistent association.
    index_rows = [
        tuple(map(int, line.split()))
        for line in Path(str(database) + '.index').read_text().splitlines()
    ]
    stride = max(1, len(index_rows) // 12)
    keys = [row[0] for row in index_rows[::stride]][:12]
    order = root / 'subset_keys'
    order.write_text(''.join(str(key) + '\n' for key in reversed(keys)))
    native_subset = root / 'subset_native'
    run(
        'createsubdb_native',
        [binary, 'createsubdb', order, database, native_subset,
         '--subdb-mode', '1', '-v', '0'])

    # A locator associates an externally built store. createsubdb propagates
    # only this relation and still creates the ordinary MMseqs data link.
    Path(str(database) + '.aastore.path').write_text(str(store) + '\n')
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
    alignment_cases = [
        ('native', native_subset, {}),
        ('full', aastore_subset, associated_environment('full')),
        ('byte_range', aastore_subset, associated_environment('byte')),
        ('prefix', aastore_subset, associated_environment('prefix', 'pread')),
    ]
    for case, source, environment in alignment_cases:
        label = 'align_subset_' + case
        result = root / label
        run(
            label,
            [binary, 'align', source, source, pref, result,
             '--threads', '4', '--min-seq-id', '0', '-e', '1000000',
             '--alignment-mode', '3', '--realign', '1', '--alt-ali', '1',
             '-a', '1', '-v', '3'],
            environment)
        value = digest(label, source, source, result)
        if baseline is None:
            baseline = value
        if value != baseline:
            raise AssertionError((label, value, baseline))
        if case != 'native':
            log = (root / (label + '.log')).read_text()
            if 'AAStore alignment:' not in log:
                raise AssertionError(
                    label + ' did not use AAStore Alignment')
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


def storage_from_case(case):
    if case == 'full':
        return 'full'
    if case.startswith('prefix'):
        return 'prefix'
    return 'byte'


if __name__ == '__main__':
    main()
