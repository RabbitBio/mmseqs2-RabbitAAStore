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

from test_aastore_exports import read_database


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

    def run(label, command, success=True):
        started = time.monotonic()
        with (root / (label + '.log')).open('w') as output:
            result = subprocess.run(
                [str(value) for value in command],
                env=base_env,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=300)
        if (result.returncode == 0) != success:
            raise AssertionError('{} returned {}; log: {}'.format(
                label, result.returncode, root / (label + '.log')))
        runs.append({
            'label': label,
            'seconds': time.monotonic() - started,
            'command': [str(value) for value in command],
        })
        (root / 'runs.json').write_text(json.dumps(runs, indent=2))
        print(label + ': passed', flush=True)
        return (root / (label + '.log')).read_text()

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
        [binary, 'createdb', fasta, database, '--shuffle', '1',
         '--createdb-mode', '0', '--dbtype', '1', '--threads', '4', '-v', '0'])
    # Keep native headers for the native-database algorithm fixtures. The first
    # AAStore Linclust run builds its sequence-only sidecar automatically.
    if store.exists():
        raise AssertionError('native createdb unexpectedly built an AAStore')

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

    if not all((store / name).is_file() for name in ('packed.bin', 'meta.bin')):
        raise AssertionError('linclust did not build the associated AAStore')

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
    if locator.read_text().splitlines()[0] != str(store):
        raise AssertionError('createsubdb did not propagate the AAStore path')
    if not Path(str(aastore_subset) + '.aastore.ids').is_file():
        raise AssertionError('createsubdb did not write its AAStore member table')
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

    # Fresh-createdb scoring must use AAStore metadata even when neither main
    # nor subset native sequence indexes can be opened. Compare raw keyed result
    # rows so the AAStore fixture does not need a native header database.
    fresh_native, fresh_aa = root / 'fresh_native', root / 'fresh_aastore'
    common = ['--shuffle', '1', '--createdb-mode', '0', '--dbtype', '1',
              '--threads', '4', '-v', '0']
    run('fresh_native_createdb', [binary, 'createdb', fasta, fresh_native,
                                 '--aastore', '0', *common])
    run('fresh_aastore_createdb', [binary, 'createdb', fasta, fresh_aa,
                                  '--aastore', '1', *common])
    if Path(str(fresh_aa) + '_h.index').exists():
        raise AssertionError('fresh AAStore scoring fixture unexpectedly has native headers')

    def write_candidates(path, query_keys, target_keys):
        offsets = []
        with path.open('wb') as output:
            for query in sorted(query_keys):
                payload = ''.join('{}\t100\t0\n'.format(target)
                                  for target in target_keys).encode() + b'\0'
                offsets.append((query, output.tell(), len(payload)))
                output.write(payload)
        Path(str(path) + '.index').write_text(''.join(
            '{}\t{}\t{}\n'.format(*entry) for entry in offsets))
        Path(str(path) + '.dbtype').write_bytes(struct.pack('<I', 7))

    def scoring_digest(result):
        records = read_database(result)
        canonical = [(key, sorted(record[:-1].splitlines()))
                     for key, record in sorted(records.items())]
        return hashlib.sha256(repr(canonical).encode()).hexdigest()

    view_keys = list(range(12))
    fresh_pref = root / 'fresh_scoring_pref'
    write_candidates(fresh_pref, view_keys, view_keys)
    selection = root / 'fresh_subset_keys'
    selection.write_text(''.join('{}\n'.format(key) for key in reversed(view_keys)))
    fresh_native_subset, fresh_aa_subset = root / 'fresh_native_subset', root / 'fresh_aastore_subset'
    for name, parent, subset in (
            ('native', fresh_native, fresh_native_subset),
            ('aastore', fresh_aa, fresh_aa_subset)):
        run('fresh_' + name + '_subset', [binary, 'createsubdb', selection, parent,
                                        subset, '--subdb-mode', '1', '-v', '0'])
    member_table = Path(str(fresh_aa_subset) + '.aastore.ids')
    if not member_table.is_file():
        raise AssertionError('fresh subset lacks its independent AAStore member table')

    modes = [
        ('align', ['--alignment-mode', '3', '--realign', '1', '--alt-ali', '1',
                   '--comp-bias-corr', '0', '-a', '1']),
        ('rescorediagonal', ['--rescore-mode', '2', '--filter-hits', '0', '--sort-results', '1']),
    ]
    stage_options = ['--min-seq-id', '0', '-e', '1000000', '-v', '3']
    baselines = {}
    for view, source in (('main', fresh_native), ('subset', fresh_native_subset)):
        for command, options in modes:
            label = 'index_independent_' + view + '_' + command + '_native'
            result = root / label
            run(label, [binary, command, source, source, fresh_pref, result,
                        '--threads', '1', *stage_options, *options])
            baselines[(view, command)] = scoring_digest(result)

    mixed_baseline = root / 'index_independent_mixed_align_native'
    run(mixed_baseline.name, [binary, 'align', fresh_native, fresh_native_subset,
                             fresh_pref, mixed_baseline, '--threads', '1',
                             *stage_options, *modes[0][1]])
    mixed_digest = scoring_digest(mixed_baseline)

    # Native database lifecycle commands must carry the compact member view.
    # Probe copied/linked/moved subsets with their own native index unavailable.
    source_sidecars = {suffix: Path(str(fresh_aa_subset) + suffix).read_bytes()
                       for suffix in ('.aastore.path', '.aastore.ids', '.index', '.dbtype')}

    def check_subset_lifecycle(label, destination):
        for suffix in ('.aastore.path', '.aastore.ids'):
            if Path(str(destination) + suffix).read_bytes() != source_sidecars[suffix]:
                raise AssertionError(label + ' did not preserve ' + suffix)
        index = Path(str(destination) + '.index')
        hidden = Path(str(index) + '.hidden')
        index.rename(hidden)
        try:
            result = root / (label + '_score')
            log = run(label + '_score', [binary, 'rescorediagonal', destination,
                      destination, fresh_pref, result, '--aastore', '1', '--threads', '1',
                      *stage_options, *modes[1][1]])
            if 'sequence_index=aastore' not in log or scoring_digest(result) != baselines[('subset', 'rescorediagonal')]:
                raise AssertionError(label + ' did not preserve index-independent subset scoring')
        finally:
            hidden.rename(index)
        checks.append({'label': label, 'member_sidecars_preserved': True,
                       'native_sequence_index_absent': True, 'native_equal': True})

    copied, linked, moved = (root / name for name in
                             ('fresh_subset_copy', 'fresh_subset_link', 'fresh_subset_moved'))
    run('lifecycle_cpdb', [binary, 'cpdb', fresh_aa_subset, copied, '-v', '0'])
    check_subset_lifecycle('lifecycle_cpdb', copied)
    run('lifecycle_lndb', [binary, 'lndb', fresh_aa_subset, linked, '-v', '0'])
    check_subset_lifecycle('lifecycle_lndb', linked)
    run('lifecycle_mvdb', [binary, 'mvdb', copied, moved, '-v', '0'])
    if any(Path(str(copied) + suffix).exists() for suffix in source_sidecars):
        raise AssertionError('mvdb left the source member view or native metadata behind')
    check_subset_lifecycle('lifecycle_mvdb', moved)
    for name, destination in (('linked', linked), ('moved', moved)):
        label = 'lifecycle_rmdb_' + name
        run(label, [binary, 'rmdb', destination, '-v', '0'])
        if any(Path(str(destination) + suffix).exists() or Path(str(destination) + suffix).is_symlink()
               for suffix in ('', *source_sidecars)):
            raise AssertionError(label + ' left database artifacts behind')
        for suffix, original in source_sidecars.items():
            if Path(str(fresh_aa_subset) + suffix).read_bytes() != original:
                raise AssertionError(label + ' changed the original subset ' + suffix)
        if not fresh_aa_subset.exists() or not (Path(str(fresh_aa) + '.aastore') / 'packed.bin').is_file():
            raise AssertionError(label + ' removed source sequence data')
        checks.append({'label': label, 'artifacts_removed': True, 'source_preserved': True})

    # Reusing a copied AAStore subset path for a native source must discard the
    # previous association, rather than keep scoring the old shared store.
    run('reuse_subset_prepare', [binary, 'cpdb', fresh_aa_subset, copied, '-v', '0'])
    run('reuse_subset_native_createdb', [binary, 'createsubdb', selection,
                                        fresh_native, copied, '--subdb-mode', '1', '-v', '0'])
    if any(Path(str(copied) + suffix).exists() for suffix in ('.aastore.path', '.aastore.ids')):
        raise AssertionError('native subset reuse retained an old AAStore association')
    reused_score = root / 'reuse_subset_native_score'
    log = run(reused_score.name, [binary, 'rescorediagonal', copied, copied,
                                 fresh_pref, reused_score, '--aastore', '0', '--threads', '1',
                                 *stage_options, *modes[1][1]])
    if 'AAStore target enabled:' in log or scoring_digest(reused_score) != baselines[('subset', 'rescorediagonal')]:
        raise AssertionError('reused native subset changed the native scoring result')
    checks.append({'label': 'reuse_subset_native', 'old_sidecars_removed': True,
                   'native_equal': True})

    # A local AAStore directory represents owned data, so createsubdb must fail
    # before it can overwrite either that store or its native sequence index.
    local_meta = Path(str(fresh_aa) + '.aastore') / 'meta.bin'
    local_index = Path(str(fresh_aa) + '.index')
    preserved_meta, preserved_index = local_meta.read_bytes(), local_index.read_bytes()
    log = run('reuse_local_store_rejected', [binary, 'createsubdb', selection,
              fresh_native, fresh_aa, '--subdb-mode', '1', '-v', '1'], success=False)
    if 'Output retains an AAStore directory' not in log or local_meta.read_bytes() != preserved_meta or local_index.read_bytes() != preserved_index:
        raise AssertionError('createsubdb did not preserve an existing local AAStore output')
    checks.append({'label': 'reuse_local_store_rejected', 'metadata_and_native_index_preserved': True})

    indexes = [Path(str(source) + '.index') for source in (fresh_aa, fresh_aa_subset)]
    hidden_indexes = [Path(str(index) + '.hidden') for index in indexes]
    for index, hidden in zip(indexes, hidden_indexes):
        index.rename(hidden)
    try:
        for view, source in (('main', fresh_aa), ('subset', fresh_aa_subset)):
            for command, options in modes:
                for threads in (1, 4):
                    label = 'index_independent_{}_{}_aastore_t{}'.format(view, command, threads)
                    result = root / label
                    log = run(label, [binary, command, source, source, fresh_pref, result,
                                      '--aastore', '1', '--threads', threads,
                                      *stage_options, *options])
                    stage = 'alignment' if command == 'align' else command
                    if 'AAStore ' + stage + ':' not in log:
                        raise AssertionError(label + ' did not use AAStore scoring')
                    if command == 'align' and 'query=aastore target=aastore' not in log:
                        raise AssertionError(label + ' did not use AAStore for both sequences')
                    value = scoring_digest(result)
                    if value != baselines[(view, command)]:
                        raise AssertionError(label + ' changed native scoring results')
                    checks.append({'label': label, 'sha256': value, 'native_equal': True,
                                   'native_sequence_index_absent': True})

        # A separate native query reader must remain independent of the AAStore
        # target buffers, including when realignment revisits target sequences.
        for threads in (1, 4):
            label = 'index_independent_mixed_align_aastore_t{}'.format(threads)
            result = root / label
            log = run(label, [binary, 'align', fresh_native, fresh_aa_subset,
                              fresh_pref, result, '--aastore', '1', '--threads', threads,
                              *stage_options, *modes[0][1]])
            if 'query=native target=aastore' not in log or scoring_digest(result) != mixed_digest:
                raise AssertionError(label + ' changed native-query/AAStore-target scoring')
            checks.append({'label': label, 'sha256': mixed_digest, 'native_equal': True,
                           'native_target_sequence_index_absent': True,
                           'native_query_reader_retained': True})

        # The key exists in the full AAStore but is excluded from this subset.
        outside = next(key for key in range(len(sequences)) if key not in view_keys)
        for missing, query_keys, target_keys in (
                ('target', [view_keys[0]], [outside]),
                ('query', [outside], [view_keys[0]])):
            bad_pref = root / ('subset_missing_' + missing + '_pref')
            write_candidates(bad_pref, query_keys, target_keys)
            for command, options in modes:
                label = 'subset_missing_' + missing + '_' + command
                log = run(label, [binary, command, fresh_aa_subset, fresh_aa_subset,
                                  bad_pref, root / label, '--aastore', '1', '--threads', '1',
                                  *stage_options, *options], success=False)
                if 'not contained in the ' + missing + ' sequence database' not in log:
                    raise AssertionError(label + ' did not report the missing subset member')
                checks.append({'label': label, 'excluded_global_key_rejected': True})

        # Removing AAStore cannot make an explicitly requested, index-free read
        # silently use the native sequence payload.
        aa_store = Path(str(fresh_aa) + '.aastore')
        hidden_store = Path(str(aa_store) + '.hidden')
        aa_store.rename(hidden_store)
        try:
            for command, options in modes:
                label = 'no_aastore_no_native_index_' + command
                run(label, [binary, command, fresh_aa, fresh_aa, fresh_pref,
                            root / label, '--aastore', '1', '--threads', '1',
                            *stage_options, *options], success=False)
                checks.append({'label': label, 'no_native_fallback': True})
        finally:
            hidden_store.rename(aa_store)
    finally:
        for index, hidden in zip(indexes, hidden_indexes):
            hidden.rename(index)

    (root / 'checks.json').write_text(json.dumps(checks, indent=2))
    print(
        'AAStore rescorediagonal, Alignment, createsubdb and native-index '
        'independence checks passed.',
        flush=True)


if __name__ == '__main__':
    main()
