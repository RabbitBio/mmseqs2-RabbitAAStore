"""Compare native and AAStore exports, including clusters spanning read batches."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess


def read_database(path):
    if path.is_file():
        data = path.read_bytes()
    else:
        shards = sorted((item for item in path.parent.glob(path.name + '.*')
                         if item.suffix[1:].isdigit()),
                        key=lambda item: int(item.suffix[1:]))
        data = b''.join(item.read_bytes() for item in shards)
    records = {}
    for line in Path(str(path) + '.index').read_text().splitlines():
        key, offset, length = map(int, line.split())
        if key in records:
            raise AssertionError('{}: duplicate record key {}'.format(path, key))
        record = data[offset:offset + length]
        if len(record) != length or not record.endswith(b'\0') or record.count(b'\0') != 1:
            raise AssertionError('{}: invalid record boundary for {}'.format(path, key))
        records[key] = record
    return records


def write_clusters(path, clusters):
    index = []
    with path.open('wb') as output:
        for key, members in clusters:
            record = ''.join(str(member) + '\n' for member in members).encode() + b'\0'
            index.append((key, output.tell(), len(record)))
            output.write(record)
    Path(str(path) + '.index').write_text(''.join(
        '{}\t{}\t{}\n'.format(*entry) for entry in sorted(index)))
    Path(str(path) + '.dbtype').write_bytes(struct.pack('<I', 6))


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

    def run(label, arguments, success=True):
        with (root / (label + '.log')).open('w') as output:
            status = subprocess.run([str(arg) for arg in arguments],
                                    env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=300)
        if (status.returncode == 0) != success:
            raise AssertionError(label + ': unexpected exit ' + str(status.returncode))
        return (root / (label + '.log')).read_text()

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        checks.append(label)
        (root / 'checks.json').write_text(json.dumps(checks, indent=2))
        print(label + ': passed', flush=True)

    fasta = root / 'input.fa'
    fasta.write_text(''.join('>s{} description {}\n{}\n'.format(
        index, index, 'ACDEFGHIKLMNPQRSTVWYBXZJUO*-' * (index % 19 + 1))
        for index in range(512)) + '>empty empty sequence\n\n')

    def compare_exports(label, database, clusters, options=(), threads=(1, 4),
                        expected_batches=None, compressed=False):
        for command in ('result2repseq', 'createseqfiledb'):
            for count in threads:
                baseline = None
                for enabled in (False, True):
                    name = '{}_{}_t{}_{}'.format(label, command, count,
                                                'aastore' if enabled else 'native')
                    output = root / name
                    extra = list(options) if command == 'createseqfiledb' else []
                    log = run(name, [binary, command, database, clusters, output,
                                    '--threads', count, '-v', '3', *extra,
                                    '--compressed', int(compressed),
                                    *(['--aastore'] if enabled else [])])
                    if compressed:
                        unpacked = root / (name + '_unpacked')
                        run(name + '_unpack', [binary, 'decompress', output, unpacked,
                                              '--threads', '1', '-v', '0'])
                        output = unpacked
                    records = read_database(output)
                    if baseline is None:
                        baseline = records
                    check(name + '_records', records == baseline)
                    check(name + '_backend', ('AAStore ' + command + ':' in log) == enabled)
                    if enabled and expected_batches and command in expected_batches:
                        check(name + '_batches', 'AAStore {} batches={}\n'.format(
                            command, expected_batches[command]) in log)

    for shuffle, compressed in ((0, 0), (1, 0), (1, 1)):
        label = 'shuffle{}_compressed{}'.format(shuffle, compressed)
        database = root / label
        run(label + '_createdb', [binary, 'createdb', fasta, database, '--aastore',
                                 '--shuffle', shuffle, '--compressed', compressed,
                                 '--dbtype', '1', '--createdb-mode', '0', '--threads', '4', '-v', '0'])
        # Physical result order, member order, and representative need not match keys.
        clusters = root / (label + '_clusters')
        write_clusters(clusters, [(8, [3, 8, 3, 2]), (1, [1]), (4, []),
                                  (3, [8, 3]), (2, [2, 1, 8])])
        compare_exports(label, database, clusters)
        compare_exports(label + '_hh', database, clusters, ('--hh-format', '1'))
        compare_exports(label + '_filtered', database, clusters,
                        ('--min-sequences', '2', '--max-sequences', '3'), threads=(1,))
        compare_exports(label + '_output_compressed', database, clusters,
                        threads=(4,), compressed=True)

        empty_keys = [int(key) for key, offset, length in
                      (line.split() for line in Path(str(database) + '.index').read_text().splitlines())
                      if int(length) == 2]
        check(label + '_empty_sequence_fixture', len(empty_keys) == 1)
        empty = root / (label + '_empty')
        write_clusters(empty, [(empty_keys[0], [empty_keys[0]]), (0, [])])
        compare_exports(label + '_empty', database, empty, threads=(1,))

        parallel = root / (label + '_parallel')
        write_clusters(parallel, [(key, [key, (key + 137) % 512, key])
                                  for key in reversed(range(512))])
        compare_exports(label + '_parallel', database, parallel, threads=(4,))

        if not compressed:
            order = root / (label + '_order')
            order.write_text('8\n3\n2\n1\n4\n')
            subset = root / (label + '_subset')
            for suffix in ('', '_h'):
                run(label + '_subset' + suffix, [binary, 'createsubdb', order,
                    str(database) + suffix, str(subset) + suffix, '--subdb-mode', '1', '-v', '0'])
            compare_exports(label + '_subset', subset, clusters)

    # Exercise thresholds without changing public options or allocating a large store.
    large_fasta = root / 'large.fa'
    batch_limit = 64 * 1024 * 1024
    large_member_bytes = 1024 * 1024 + 1 + len(b's0 description\n\0')
    small_member_bytes = len(b'WQ*X-\0') + len(b's2 description\n\0')
    # A preceding cluster leaves exactly one small member's worth of room.
    padding_bytes = (batch_limit - 63 * large_member_bytes - small_member_bytes
                     - len(b'WQ*X-\0') - len(b's72 \n\0'))
    with large_fasta.open('w') as output:
        for index in range(72):
            header = 'd' * (512 * 1024) if index == 1 else 'description'
            sequence = 'ACDEFGHI' * (128 * 1024) if index == 0 else 'WQ*X-'
            output.write('>s{} {}\n{}\n'.format(index, header, sequence))
        output.write('>s72 {}\nWQ*X-\n'.format('d' * padding_bytes))
    database = root / 'large'
    run('large_createdb', [binary, 'createdb', large_fasta, database, '--aastore',
                          '--shuffle', '0', '--createdb-mode', '0', '--dbtype', '1',
                          '--threads', '4', '-v', '0'])
    clusters = root / 'header_boundary'
    write_clusters(clusters, [(1, [1] * 130), (2, [2]), (3, [])])
    compare_exports('header_boundary', database, clusters, threads=(1,),
                    expected_batches={'createseqfiledb': 2})
    clusters = root / 'giant_cluster'
    write_clusters(clusters, [(2, [2]), (1, [1] * 260), (3, [3])])
    compare_exports('giant_cluster', database, clusters,
                    expected_batches={'createseqfiledb': 3})
    compare_exports('giant_cluster_hh', database, clusters, ('--hh-format', '1'),
                    threads=(1,), expected_batches={'createseqfiledb': 3}, compressed=True)

    clusters = root / 'giant_sequence_cluster'
    write_clusters(clusters, [(0, [0] * 130), (2, [2])])
    compare_exports('giant_sequence_cluster', database, clusters, threads=(1,),
                    expected_batches={'createseqfiledb': 3}, compressed=True)

    clusters = root / 'parallel_giant'
    write_clusters(clusters, [(600, [1] * 260)] +
                   [(key, [key % 70 + 2]) for key in range(512)])
    compare_exports('parallel_giant', database, clusters, threads=(4,))

    clusters = root / 'small_first_fragment'
    write_clusters(clusters, [(72, [0] * 63 + [72]), (2, [2] + [1] * 130), (3, [3])])
    compare_exports('small_first_fragment', database, clusters, threads=(1,),
                    expected_batches={'createseqfiledb': 3}, compressed=True)
    compressed_output = root / 'small_first_fragment_createseqfiledb_t1_aastore'
    payload = compressed_output.read_bytes()
    for key, offset, length in (map(int, line.split()) for line in
            Path(str(compressed_output) + '.index').read_text().splitlines()):
        if key == 2:
            compressed_length = struct.unpack_from('<I', payload, offset)[0]
            check('small_first_fragment_stays_compressed',
                  payload[offset + 4 + compressed_length] == 0)

    clusters = root / 'exact_cluster_boundary'
    write_clusters(clusters, [(72, [0] * 63 + [72, 2]), (3, [3])])
    compare_exports('exact_cluster_boundary', database, clusters, threads=(1,),
                    expected_batches={'createseqfiledb': 2})

    clusters = root / 'sequence_boundary'
    write_clusters(clusters, [(key, [0]) for key in range(70)])
    compare_exports('sequence_boundary', database, clusters, threads=(1,),
                    expected_batches={'result2repseq': 2, 'createseqfiledb': 2})

    # AAStore sequence reads must work without the native sequence payload.
    native_payload = root / 'large.native_payload'
    database.rename(native_payload)
    try:
        for command in ('result2repseq', 'createseqfiledb'):
            name = 'without_native_' + command
            result = root / name
            run(name, [binary, command, database, clusters, result, '--aastore',
                       '--threads', '1', '-v', '0'])
            check(name, read_database(result) == read_database(
                root / ('sequence_boundary_' + command + '_t1_native')))
    finally:
        native_payload.rename(database)

    # A sparse result file exhausts the automatic prefix reserve without reading
    # or allocating its unused extent, exercising disk batches on small fixtures.
    original_size = clusters.stat().st_size
    sparse_size = os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE') * 2
    try:
        with clusters.open('r+b') as output:
            output.truncate(sparse_size)
        for command in ('result2repseq', 'createseqfiledb'):
            name = 'disk_' + command
            output = root / name
            log = run(name, [binary, command, database, clusters, output, '--aastore',
                             '--threads', '4', '--db-load-mode', '2', '-v', '3'])
            check(name + '_mode', 'storage_mode=byte_range' in log)
            check(name + '_records', read_database(output) == read_database(
                root / ('sequence_boundary_' + command + '_t1_native')))
    finally:
        with clusters.open('r+b') as output:
            output.truncate(original_size)

    # Check an open, multi-batch cluster on the disk-backed path as well.
    clusters = root / 'giant_cluster'
    original_size = clusters.stat().st_size
    try:
        with clusters.open('r+b') as output:
            output.truncate(sparse_size)
        result = root / 'disk_giant_cluster'
        log = run('disk_giant_cluster', [binary, 'createseqfiledb', database, clusters,
                  result, '--aastore', '--threads', '4', '--db-load-mode', '2', '-v', '3'])
        check('disk_giant_cluster_mode', 'storage_mode=byte_range' in log)
        check('disk_giant_cluster_batches', 'AAStore createseqfiledb batches=3\n' in log)
        check('disk_giant_cluster_records', read_database(result) == read_database(
              root / 'giant_cluster_createseqfiledb_t1_native'))
    finally:
        with clusters.open('r+b') as output:
            output.truncate(original_size)

    print('{} export checks passed; logs: {}'.format(len(checks), root))


if __name__ == '__main__':
    main()
