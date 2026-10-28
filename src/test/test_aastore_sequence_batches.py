"""Compare native/RabbitAAStore scoring with complete-query candidate-byte batches.

Repeated inexpensive coverage rejects reach the 64 MiB candidate-byte budget
without decoding a large sequence database. Check that multiple queries share
a batch and that an oversized query stays complete. A sparse, unindexed file
tail forces actual NoMemory reads without allocating its logical extent.
"""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
from test_aastore_exports import read_database


QUERY_COUNT = 20
BATCH_BYTES = 64 * 1024 * 1024
SEQUENCE = 'MQIFVKTLTGKTITLEVEPSDTIENVKAKIQDKE'


def write_prefilter(path, oversized=False):
    # The first accepted hit has a larger key than the subsequent hits: a
    # sorted/precollected scoring loop would violate max-accept=1 behavior.
    first = QUERY_COUNT + 3
    rejected = QUERY_COUNT + 4
    first_hit = (str(first) + '\t100\t0\n').encode()
    repeats = 650000 if oversized else 65535
    prefix = first_hit + (str(rejected) + '\t0\t0\n').encode() * repeats
    # Candidate occurrences count toward the byte budget even when decoding
    # deduplicates their IDs. Repeated accepted candidates remain in the output.
    suffix = ''.join('{}\t100\t0\n'.format(key) for key in
                     (QUERY_COUNT, rejected, QUERY_COUNT + 1, QUERY_COUNT)).encode()
    record = prefix + suffix + b'\0'
    index = []
    with path.open('wb') as output:
        for key in range(QUERY_COUNT):
            payload = first_hit + b'\0' if oversized and key > 0 else record
            index.append((key, output.tell(), len(payload)))
            output.write(payload)
    Path(str(path) + '.index').write_text(''.join(
        '{}\t{}\t{}\n'.format(*entry) for entry in index))
    Path(str(path) + '.dbtype').write_bytes(struct.pack('<I', 7))
    short_bytes = len(SEQUENCE) + 1
    rejected_bytes = len(SEQUENCE) * 4 + 1
    complete_query_bytes = (repeats + 1) * rejected_bytes + 4 * short_bytes
    if oversized:
        # The first query exceeds the budget and forms one complete batch;
        # the remaining 19 small queries share the next batch.
        return (2, repeats + 5 + QUERY_COUNT - 1, complete_query_bytes)
    # Eight complete queries reach the budget, giving groups of 8, 8, and 4.
    assert 7 * complete_query_bytes < BATCH_BYTES <= 8 * complete_query_bytes
    return (3, QUERY_COUNT * (repeats + 5), 8 * complete_query_bytes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    binary = args.mmseqs.resolve()
    root = args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, LC_ALL='C')
    checks = []

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        checks.append(label)
        (root / 'checks.json').write_text(json.dumps(checks, indent=2))
        print(label + ': passed', flush=True)

    def run(label, arguments, success=True):
        log_path = root / (label + '.log')
        with log_path.open('w') as output:
            status = subprocess.run([str(value) for value in arguments],
                                    env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=300)
        if (status.returncode == 0) != success:
            raise AssertionError('{}: unexpected exit {}; see {}'.format(
                label, status.returncode, log_path))
        return log_path.read_text()

    fasta = root / 'input.fa'
    fasta.write_text(''.join('>s{}\n{}\n'.format(index, SEQUENCE)
                            for index in range(QUERY_COUNT + 4)) +
                     '>coverage_reject\n' + SEQUENCE * 4 + '\n')
    databases = {}
    for enabled in (False, True):
        name = 'aastore' if enabled else 'native'
        database = root / name
        databases[enabled] = database
        run(name + '_createdb', [binary, 'createdb', fasta, database,
                                '--aastore', int(enabled), '--shuffle', '0',
                                '--createdb-mode', '0', '--dbtype', '1',
                                '--threads', '4', '-v', '0'])
    check('fixture_sequence_ids', read_database(databases[False]) ==
          read_database(databases[True]))

    # DBReader only accesses indexed records. Its file-size reserve drives the
    # production automatic policy to NoMemory; mmap mode avoids touching the
    # sparse tail. Never read this input with read_bytes/read_database.
    sparse_size = (os.sysconf('SC_PHYS_PAGES') *
                   os.sysconf('SC_PAGE_SIZE') * 2)
    if sys.maxsize < sparse_size or sparse_size >= (1 << 46):
        raise RuntimeError('This sparse-mmap fixture requires a 64-bit host '
                           'with less than 32 TiB physical memory')

    common = ['--db-load-mode', '2', '-v', '3', '--min-seq-id', '0.9',
              '-c', '0.9', '--cov-mode', '0', '-e', '1000', '-a', '1']

    # The adapter must preserve native error handling before coverage checks:
    # a nonexistent target cannot become a zero-length sequence and be skipped.
    missing_pref = root / 'missing_target_pref'
    missing_record = b'999999\t100\t0\n\0'
    missing_pref.write_bytes(missing_record)
    Path(str(missing_pref) + '.index').write_text(
        '0\t0\t{}\n'.format(len(missing_record)))
    Path(str(missing_pref) + '.dbtype').write_bytes(struct.pack('<I', 7))
    missing_query_pref = root / 'missing_query_pref'
    missing_query_record = b'0\t100\t0\n\0'
    missing_query_pref.write_bytes(missing_query_record)
    Path(str(missing_query_pref) + '.index').write_text(
        '999999\t0\t{}\n'.format(len(missing_query_record)))
    Path(str(missing_query_pref) + '.dbtype').write_bytes(struct.pack('<I', 7))
    # A one-byte data file permits mmap, while the empty index has no queries.
    empty_pref = root / 'empty_pref'
    empty_pref.write_bytes(b'\0')
    Path(str(empty_pref) + '.index').write_text('')
    Path(str(empty_pref) + '.dbtype').write_bytes(struct.pack('<I', 7))
    for enabled in (False, True):
        mode = 'aastore' if enabled else 'native'
        database = databases[enabled]
        label = 'missing_target_' + mode
        log = run(label, [binary, 'rescorediagonal', database, database,
                          missing_pref, root / label, '--threads', '1',
                          '--aastore', int(enabled), '--rescore-mode', '2',
                          '--filter-hits', '0', *common], success=False)
        check(label + '_clear_error_before_coverage',
              'Sequence 999999' in log and
              'not contained in the target sequence database' in log)
        if enabled:
            check(label + '_actual_full_memory', 'storage_mode=full_memory' in log)
        label = 'missing_query_' + mode
        log = run(label, [binary, 'rescorediagonal', database, database,
                          missing_query_pref, root / label, '--threads', '1',
                          '--aastore', int(enabled), '--rescore-mode', '2',
                          '--filter-hits', '0', *common], success=False)
        if enabled:
            check(label + '_clear_query_error',
                  'Query sequence 999999' in log and
                  'not contained in the query sequence database' in log)
            check(label + '_actual_full_memory', 'storage_mode=full_memory' in log)
        else:
            check(label + '_native_invalid_database_error',
                  'Invalid database read' in log or 'local id' in log)
        label = 'empty_pref_' + mode
        result = root / label
        log = run(label, [binary, 'rescorediagonal', database, database,
                          empty_pref, result, '--threads', '1',
                          '--aastore', int(enabled), '--rescore-mode', '2',
                          '--filter-hits', '0', *common])
        check(label + '_empty_result', read_database(result) == {})
        if enabled:
            check(label + '_actual_full_memory', 'storage_mode=full_memory' in log)

    align = ['--alignment-mode', '3', '--comp-bias-corr', '0',
             '--max-seq-len', '2048']
    unlimited = ['--max-accept', '100', '--max-rejected', '1000000']
    cases = [
        ('align_all', 'align', align + unlimited, False, False),
        ('align_accept_stop', 'align', align +
         ['--max-accept', '1', '--max-rejected', '100000'], True, False),
        ('align_reject_stop', 'align', align +
         ['--max-accept', '100', '--max-rejected', '2'], True, False),
        ('align_alternative', 'align', align + unlimited +
         ['--alt-ali', '1'], False, True),
        ('align_realign_alternative', 'align', align + unlimited +
         ['--realign', '1', '--realign-score-bias', '0',
          '--realign-max-seqs', '100', '--alt-ali', '1'], False, True),
        ('rescore_hamming', 'rescorediagonal',
         ['--rescore-mode', '0', '--filter-hits', '0', '--sort-results', '1'],
         False, False),
        ('rescore_alignment', 'rescorediagonal',
         ['--rescore-mode', '2', '--filter-hits', '0', '--sort-results', '1'],
         False, False),
    ]

    for oversized in (False, True):
        fixture = 'oversized_complete_query' if oversized else 'multiple_complete_queries'
        pref = root / (fixture + '_pref')
        expected_batches, expected_occurrences, expected_maximum = write_prefilter(
            pref, oversized)
        original_size = pref.stat().st_size
        # The standard fixture exercises the full option matrix. The oversized
        # fixture checks both scoring commands without making one query split.
        selected = [cases[0], cases[-1]] if oversized else cases
        try:
            with pref.open('r+b') as output:
                output.truncate(sparse_size)
            check(fixture + '_sparse_tail_uses_only_fixture_blocks',
                  pref.stat().st_blocks * 512 < original_size * 2 + 1024 * 1024)
            for case, command, options, stops_early, revisits in selected:
                baseline = None
                for threads in (1, 4):
                    for enabled in (False, True):
                        label = '{}_{}_t{}_{}'.format(
                            fixture, case, threads, 'aastore' if enabled else 'native')
                        database = databases[enabled]
                        result = root / label
                        log = run(label, [binary, command, database, database,
                                          pref, result, '--threads', threads,
                                          '--aastore', int(enabled), *common, *options])
                        records = read_database(result)
                        if baseline is None:
                            baseline = records
                        check(label + '_native_records', records == baseline)
                        check(label + '_query_keys', set(records) == set(range(QUERY_COUNT)))
                        if stops_early:
                            check(label + '_original_candidate_order', all(
                                [int(row.split(b'\t', 1)[0]) for row in
                                 record[:-1].splitlines()] == [QUERY_COUNT + 3]
                                for record in records.values()))
                        elif not revisits:
                            check(label + '_duplicate_candidates_preserved', all(
                                sorted(int(row.split(b'\t', 1)[0]) for row in
                                       record[:-1].splitlines()) ==
                                ([QUERY_COUNT + 3] if oversized and key > 0 else
                                 [QUERY_COUNT, QUERY_COUNT, QUERY_COUNT + 1, QUERY_COUNT + 3])
                                for key, record in records.items()))
                        if not enabled:
                            continue
                        stage = 'alignment' if command == 'align' else command
                        check(label + '_actual_no_memory',
                              'storage_mode=no_memory' in log)
                        check(label + '_candidate_byte_batch_policy',
                              'RabbitAAStore {}: candidate-byte batches'.format(stage) in log and
                              'candidate_bytes_per_batch={}'.format(BATCH_BYTES) in log)
                        stats = re.search(r'RabbitAAStore ' + stage +
                                          r' batches=(\d+) candidate_occurrences=(\d+) '
                                          r'max_candidate_bytes=(\d+)', log)
                        check(label + '_batch_statistics', stats is not None)
                        batches, occurrences, maximum = map(int, stats.groups())
                        # Stopping scoring early does not change prefetch: the
                        # same complete queries were collected before scoring.
                        check(label + '_complete_query_batch_count',
                              batches == expected_batches)
                        check(label + '_all_candidate_occurrences_prefetched',
                              occurrences == expected_occurrences)
                        check(label + '_complete_query_byte_budget',
                              maximum == expected_maximum)
        finally:
            with pref.open('r+b') as output:
                output.truncate(original_size)

    print('Verified {} checks; actual NoMemory scoring used shared batches '
          'and preserved oversized complete queries.'.format(len(checks)), flush=True)


if __name__ == '__main__':
    main()
