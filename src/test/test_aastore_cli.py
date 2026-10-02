"""Verify automatic AAStore construction, ID order, and workflow propagation."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--builder', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    binary = args.mmseqs.resolve()
    builder = args.builder.resolve()
    root = args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith('MMSEQS_AASTORE')}
    environment['LC_ALL'] = 'C'
    checks = []

    def run(label, arguments, overrides=None, success=True):
        env = dict(environment)
        env.update(overrides or {})
        with (root / (label + '.log')).open('w') as output:
            result = subprocess.run([str(value) for value in arguments], env=env,
                                    stdout=output, stderr=subprocess.STDOUT,
                                    timeout=300)
        if (result.returncode == 0) != success:
            raise AssertionError(label + ': unexpected status ' +
                                 str(result.returncode))
        return (root / (label + '.log')).read_text()

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        checks.append(label)
        (root / 'checks.json').write_text(json.dumps(checks, indent=2))
        print(label + ': passed', flush=True)

    def compare_store(label, database):
        # Use the public FASTA builder as an independent oracle for every key.
        exported = root / (label + '_export.fa')
        expected = root / (label + '_expected')
        run(label + '_export', [binary, 'convert2fasta', database, exported, '-v', '0'])
        run(label + '_expected', [builder, exported, expected,
                                  '--sequences-only', '--threads', '1'])
        actual = Path(str(database) + '.aastore')
        for name in ('packed.bin', 'meta.bin'):
            check(label + '_' + name,
                  (actual / name).read_bytes() == (expected / name).read_bytes())
        check(label + '_no_temporary_store', not list(database.parent.glob(
            database.name + '.aastore.tmp.*')))

    rng = random.Random(991)
    alphabet = 'ACDEFGHIKLMNPQRSTVWY'
    records = []
    for family in range(12):
        sequence = ''.join(rng.choice(alphabet) for _ in range(240 + family))
        for variant in (sequence, sequence, sequence[:80] + 'WQWQW' + sequence[80:]):
            records.append('>seq{}\n{}\n'.format(len(records), variant))
    fasta = root / 'input.fa'
    fasta.write_text(''.join(records))
    compressed = root / 'input.fa.gz'
    with gzip.open(compressed, 'wt') as output:
        output.write(fasta.read_text())
    first = root / 'first.fa'
    second = root / 'second.fa'
    first.write_text(''.join(records[:18]))
    second.write_text(''.join(records[18:]))
    multiline = root / 'multiline.fa'
    multiline.write_text(''.join(record[:record.index('\n') + 1] +
                        record[record.index('\n') + 1:-1][:100] + '\n' +
                        record[record.index('\n') + 1:-1][100:] + '\n'
                        for record in records))

    for label, inputs, options in (
            ('ordered', [fasta], ['--shuffle', '0', '--createdb-mode', '0']),
            ('shuffled', [fasta], ['--shuffle', '1', '--createdb-mode', '0']),
            ('soft', [fasta], ['--shuffle', '1', '--createdb-mode', '1']),
            ('soft_fallback', [multiline], ['--shuffle', '1', '--createdb-mode', '1']),
            ('compressed', [fasta], ['--shuffle', '1', '--compressed', '1']),
            ('gzip', [compressed], ['--shuffle', '1']),
            ('multiple_inputs', [first, second], ['--shuffle', '1'])):
        database = root / label
        run(label + '_createdb', [binary, 'createdb', '--aastore', *inputs,
                                  database, '--dbtype', '1', '--threads', '4',
                                  '-v', '3', *options])
        compare_store(label, database)
    check('shuffle_changes_order', (root / 'ordered.aastore/packed.bin').read_bytes()
          != (root / 'shuffled.aastore/packed.bin').read_bytes())

    boundary = root / 'boundary.fa'
    boundary.write_text(''.join('>b{}\n{}\n'.format(index, 'ACDEFG' * (index % 19 + 1))
                                for index in range(1031)) +
                        '>long\n' + 'WQ' * (1024 * 1024 + 3) + '\n')
    run('boundary_createdb', [binary, 'createdb', boundary, root / 'boundary',
                              '--dbtype', '1', '--aastore', '--threads', '4', '-v', '0'])
    compare_store('boundary', root / 'boundary')

    # Rebuilding createdb must refresh an old local AAStore, not retain stale IDs.
    replacement = root / 'replacement.fa'
    replacement.write_text('>new\nWQWQWACDEFG\n>another\nLLLLM*X-\n')
    rebuilt = root / 'rebuilt'
    run('rebuild_first', [binary, 'createdb', fasta, rebuilt, '--aastore',
                          '--dbtype', '1', '-v', '0'])
    run('rebuild_second', [binary, 'createdb', replacement, rebuilt, '--aastore',
                           '--dbtype', '1', '-v', '0'])
    compare_store('rebuilt', rebuilt)

    run('native_rebuild', [binary, 'createdb', fasta, rebuilt, '--dbtype', '1',
                           '--aastore', '0', '-v', '0'])
    log = run('refresh_stale_store', [binary, 'linclust', rebuilt,
              root / 'refresh_result', root / 'refresh_tmp', '--threads', '4',
              '--aastore', '-v', '3'])
    check('stale_local_store_rebuilt', 'Building AAStore' in log)
    compare_store('refreshed', rebuilt)

    for label, options in (
            ('reject_id_offset', ['--id-offset', '7']),
            ('reject_gpu', ['--createdb-mode', '2']),
            ('reject_nucleotide', ['--dbtype', '2'])):
        database = root / label
        run(label, [binary, 'createdb', fasta, database, '--aastore',
                     '-v', '1', *options], success=False)
        check(label + '_no_output', not Path(str(database) + '.dbtype').exists())

    def digest(path):
        return hashlib.sha256(b'\n'.join(sorted(path.read_bytes().splitlines()))).hexdigest()

    storage_modes = {
        'full': {'MMSEQS_AASTORE_FULL_MEMORY': '1'},
        'byte': {'MMSEQS_AASTORE_FULL_MEMORY': '0',
                 'MMSEQS_AASTORE_CACHE_BYTES': '0'},
    }
    database = root / 'shuffled'
    baseline = None
    packed = root / 'shuffled.aastore/packed.bin'
    before = packed.stat().st_mtime_ns
    for label, overrides in [('native', {})] + list(storage_modes.items()):
        result = root / ('direct_' + label)
        arguments = [binary, 'linclust', database, result,
                     root / ('direct_' + label + '_tmp'), '--threads', '4',
                     '--min-seq-id', '0.9', '-c', '0.9', '--cov-mode', '0',
                     '--cluster-mode', '3', '-v', '3', '--aastore']
        if label == 'native':
            arguments.append('0')
        log = run('direct_' + label, arguments, overrides)
        tsv = root / ('direct_' + label + '.tsv')
        run('direct_' + label + '_tsv', [binary, 'createtsv', database,
                                       database, result, tsv, '-v', '0'])
        value = digest(tsv)
        if baseline is None:
            baseline = value
        check('direct_' + label + '_results', value == baseline)
        check('direct_' + label + '_reader',
              ('AAStore target enabled:' in log) == (label != 'native'))
    check('existing_store_not_rebuilt', packed.stat().st_mtime_ns == before)

    missing = root / 'missing_store'
    run('missing_createdb', [binary, 'createdb', fasta, missing, '--dbtype', '1', '-v', '0'])
    run('missing_linclust', [binary, 'linclust', '--aastore', missing,
                             root / 'missing_result', root / 'missing_tmp',
                             '--threads', '4', '-v', '3'])
    compare_store('linclust_builds_missing', missing)

    gapped = root / 'gapped_keys'
    run('gapped_createdb', [binary, 'createdb', fasta, gapped, '--id-offset', '7',
                           '--shuffle', '0', '--dbtype', '1', '-v', '0'])
    run('gapped_linclust', [binary, 'linclust', gapped, root / 'gapped_result',
                            root / 'gapped_tmp', '--aastore', '-v', '1'], success=False)
    check('gapped_keys_rejected', not Path(str(gapped) + '.aastore').exists())

    # Easy workflows must forward --aastore through both creation and cascade.
    for workflow, extra in (
            ('easy-linclust', ['--createdb-mode', '0', '--shuffle', '1']),
            ('easy-cluster', ['--cluster-steps', '2', '-s', '1'])):
        baseline = None
        for enabled in (False, True):
            label = workflow + ('_aastore' if enabled else '_native')
            prefix = root / label
            tmp = root / (label + '_tmp')
            arguments = [binary, workflow, fasta, prefix, tmp, '--threads', '4',
                         '--min-seq-id', '0.9', '-c', '0.9', '--cov-mode', '0',
                         '--cluster-mode', '3', '--remove-tmp-files', '0',
                         '-v', '3', *extra, '--aastore', '1' if enabled else '0']
            log = run(label, arguments, storage_modes['byte'])
            value = digest(Path(str(prefix) + '_cluster.tsv'))
            if baseline is None:
                baseline = value
            check(label + '_results', value == baseline)
            check(label + '_reader', ('AAStore target enabled:' in log) == enabled)
            if enabled:
                stores = {store.resolve() for store in tmp.glob('*/input.aastore')}
                check(label + '_automatic_store', len(stores) == 1)
                for store in stores:
                    compare_store(label, Path(str(store)[:-len('.aastore')]))
                commands = [shlex.split(line) for line in log.splitlines()
                            if line.startswith('clust ')]
                check(label + '_cluster_mode', bool(commands) and all(
                    command[command.index('--cluster-mode') + 1] == '3'
                    for command in commands))

    cleanup_tmp = root / 'cleanup_tmp'
    run('cleanup', [binary, 'easy-linclust', fasta, root / 'cleanup',
                     cleanup_tmp, '--threads', '4', '--aastore', '-v', '3'])
    check('easy_cleanup', not list(cleanup_tmp.glob('*/input.aastore*')))
    print('{} automatic CLI checks passed; logs: {}'.format(len(checks), root))


if __name__ == '__main__':
    main()
