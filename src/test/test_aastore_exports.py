"""Compare fresh AAStore direct FASTA and TSV exports with native output."""

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


def same_file(left, right):
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open('rb') as expected, right.open('rb') as actual:
        while True:
            chunk = expected.read(1024 * 1024)
            if chunk != actual.read(1024 * 1024):
                return False
            if not chunk:
                return True


def text_column(store, name):
    """Read AATEXT1 independently so construction has a byte-level oracle."""
    metadata = (store / (name + '.meta')).read_bytes()
    if metadata[:8] != b'AATEXT1\0':
        raise AssertionError('invalid text-column magic')
    version, count, size, shift, blocks, offsets = struct.unpack_from(
        '<6Q', metadata, 8)
    if version != 1 or offsets != count + 1:
        raise AssertionError('invalid text-column dimensions')
    if blocks != (count + (1 << shift) - 1) >> shift:
        raise AssertionError('invalid text-column blocks')
    if len(metadata) != 56 + offsets * 4 + (blocks + 1) * 8:
        raise AssertionError('invalid text-column metadata size')
    relative = struct.unpack_from('<{}I'.format(offsets), metadata, 56)
    bases = struct.unpack_from('<{}Q'.format(blocks + 1), metadata,
                               56 + offsets * 4)
    absolute = [bases[index >> shift] + relative[index]
                for index in range(offsets)]
    data = (store / (name + '.bin')).read_bytes()
    if len(data) != size or absolute[0] != 0 or absolute[-1] != size:
        raise AssertionError('invalid text-column data size')
    if absolute != sorted(absolute):
        raise AssertionError('invalid text-column boundaries')
    return [data[absolute[index]:absolute[index + 1]]
            for index in range(count)]


def replace_headers(database, headers):
    """Make native-header edge cases that FASTA parsers might normalize."""
    path = Path(str(database) + '_h')
    entries = []
    with path.open('wb') as output:
        for key, header in sorted(headers.items()):
            record = header + b'\n\0'
            entries.append((key, output.tell(), len(record)))
            output.write(record)
    Path(str(path) + '.index').write_text(''.join(
        '{}\t{}\t{}\n'.format(*entry) for entry in entries))
    Path(str(path) + '.dbtype').write_bytes(struct.pack('<I', 12))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    binary, root = args.mmseqs.resolve(), args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, LC_ALL='C')
    checks = []

    def run(label, arguments, success=True):
        log = root / (label + '.log')
        with log.open('w') as output:
            status = subprocess.run([str(binary), *map(str, arguments)],
                                    env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=300)
        if (status.returncode == 0) != success:
            raise AssertionError('{}: unexpected exit {}; see {}'.format(
                label, status.returncode, log))
        return log.read_text()

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        checks.append(label)
        (root / 'checks.json').write_text(json.dumps(checks, indent=2))
        print(label + ': passed', flush=True)

    def compare_fasta(label, native, aa, clusters, threads=1,
                      representative=False, hh_format=False, options=()):
        body, expected, actual = (root / (label + suffix)
                                  for suffix in ('_body', '_native.fa', '_aa.fa'))
        # Native commands remain an independent oracle; they are never asked
        # to read AAStore and are not used by the AAStore production workflow.
        command = 'result2repseq' if representative else 'createseqfiledb'
        body_options = [] if representative else ['--hh-format', int(hh_format), *options]
        run(label + '_native_body', [command, native, clusters, body,
                                     '--threads', 1, '-v', 0, *body_options])
        flat_options = ['--use-fasta-header', 1] if representative else []
        run(label + '_native_flat', ['result2flat', native, native, body, expected,
                                    '-v', 0, *flat_options])
        log = run(label + '_direct', ['aastoreexportfasta', aa, clusters, actual,
                  '--aastore', 1, '--threads', threads,
                  '--representative-only', int(representative),
                  '--hh-format', int(hh_format), '-v', 3, *options])
        check(label + '_bytes', same_file(expected, actual))
        check(label + '_records_backend',
              'AAStore aastoreexportfasta: batch_get_records fields=all header=AAStore' in log)
        check(label + '_dense_ids', 'records_dense_ids=1' in log)
        return expected

    def compare_tsv(label, native, aa, clusters, options=()):
        expected, actual = root / (label + '_native.tsv'), root / (label + '_aa.tsv')
        run(label + '_native', ['createtsv', native, native, clusters, expected,
                               '--threads', 1, '-v', 0, *options])
        log = run(label + '_aa', ['createtsv', aa, aa, clusters, actual,
                  '--aastore', 1, '--threads', 4, '-v', 3, *options])
        check(label + '_bytes', same_file(expected, actual))
        check(label + '_text_backend', 'fields=name,description native_header_db=0' in log)

    fasta = root / 'input.fa'
    alphabet = 'ACDEFGHIKLMNPQRSTVWY'
    fasta.write_text(''.join('>sp|P{:05d}|E{}\t\tdescription {}  \n{}\n'.format(
        index, index, index, alphabet * (index % 5 + 1)) for index in range(128)) +
        '>empty empty sequence\n\n')

    for shuffle, compressed in ((0, 0), (1, 0), (1, 1)):
        label = 'shuffle{}_compressed{}'.format(shuffle, compressed)
        native, aa = root / (label + '_native'), root / (label + '_aa')
        common = ['--shuffle', shuffle, '--compressed', compressed,
                  '--dbtype', 1, '--createdb-mode', 0, '--threads', 4, '-v', 0]
        run(label + '_native_createdb', ['createdb', fasta, native, '--aastore', 0, *common])
        run(label + '_aa_createdb', ['createdb', fasta, aa, '--aastore', 1, *common])
        check(label + '_header_free', not list(root.glob(aa.name + '_h*')))
        store = Path(str(aa) + '.aastore')
        names, descriptions = text_column(store, 'names'), text_column(store, 'descriptions')
        check(label + '_field_count', len(names) == 129 and len(descriptions) == 129)
        # Default AAStore fields normalize the name/description separator.
        # Apply only that documented normalization to the native oracle.
        replace_headers(native, {key: name + (b' ' if descriptions[key] else b'') + descriptions[key]
                                 for key, name in enumerate(names)})

        if not shuffle and not compressed:
            # Simulate an old main-store certificate and one-line association.
            # Its first successful export must upgrade both, preserving fields.
            marker = store / 'mmseqs_headers.lossless'
            lines = marker.read_text().splitlines()
            check('fresh_record_certificate_v2', lines[0] == 'MMSEQS_AASTORE_HEADERS_V2')
            fields = dict(line.split('=', 1) for line in lines[1:])
            old_fields = {key: fields[key] for key in
                          ('origin', 'dense_keys', 'header_newline', 'reconstruct', 'records')}
            source_index = Path(str(aa) + '.index').stat()
            old_fields.update(source_index_size=str(source_index.st_size),
                              source_index_inode=str(source_index.st_ino),
                              source_index_mtime_ns=str(source_index.st_mtime_ns),
                              packed_size=str((store / 'packed.bin').stat().st_size),
                              sequence_meta_size=str((store / 'meta.bin').stat().st_size),
                              names_bytes=str((store / 'names.bin').stat().st_size),
                              descriptions_bytes=str((store / 'descriptions.bin').stat().st_size))
            marker.write_text('MMSEQS_AASTORE_HEADERS_V1\n' + ''.join(
                '{}={}\n'.format(key, value) for key, value in old_fields.items()))
            locator = Path(str(aa) + '.aastore.path')
            full_locator = locator.read_text().splitlines() if locator.exists() else None
            locator.write_text((full_locator[0] if full_locator else str(store)) + '\n')

        clusters = root / (label + '_clusters')
        write_clusters(clusters, [(8, [3, 8, 3, 2]), (1, [1]), (4, []),
                                  (3, [8, 3]), (2, [2, 1, 8]), (9, [2, 1])])
        for count in (1, 4):
            compare_fasta(label + '_all_t' + str(count), native, aa, clusters, threads=count)
            if not shuffle and not compressed and count == 1:
                upgraded = marker.read_text().splitlines()
                check('legacy_record_certificate_upgraded_v2',
                      upgraded[0] == 'MMSEQS_AASTORE_HEADERS_V2' and
                      'reconstruct=' + old_fields['reconstruct'] in upgraded and
                      not any(line.startswith('source_index_') for line in upgraded))
                check('legacy_record_association_persisted_full',
                      locator.read_text().splitlines() == full_locator if full_locator
                      else not locator.exists())
            compare_fasta(label + '_rep_t' + str(count), native, aa, clusters,
                          threads=count, representative=True)
        compare_fasta(label + '_hh', native, aa, clusters, threads=4, hh_format=True)
        compare_fasta(label + '_filtered', native, aa, clusters,
                      options=('--min-sequences', 2, '--max-sequences', 3))
        for suffix, options in [('default', ()), ('full', ('--full-header', 1)),
                                ('first', ('--first-seq-as-repr', 1)),
                                ('numeric', ('--target-column', 0))]:
            compare_tsv(label + '_tsv_' + suffix, native, aa, clusters, options)

        empty_key = names.index(b'empty')
        empty = root / (label + '_empty')
        write_clusters(empty, [(empty_key, [empty_key]), (0, [])])
        compare_fasta(label + '_empty_all', native, aa, empty, threads=4)
        compare_fasta(label + '_empty_rep', native, aa, empty,
                      threads=4, representative=True)
        compare_tsv(label + '_empty_tsv', native, aa, empty)

        parallel = root / (label + '_parallel')
        write_clusters(parallel, [(key, [key, (key + 37) % 128, key])
                                  for key in reversed(range(128))])
        expected = compare_fasta(label + '_parallel', native, aa, parallel, threads=4)
        # Both FASTA and TSV must work with only AAStore source fields and the
        # cluster/result index. No native sequence index, payload, or header is
        # available to validate or supply the source records.
        payloads = ([aa] if aa.is_file() else sorted(
            item for item in aa.parent.glob(aa.name + '.*') if item.suffix[1:].isdigit()))
        index = Path(str(aa) + '.index')
        check(label + '_native_payload_fixture', bool(payloads) and index.is_file())
        moved = []
        try:
            for artifact in [index, *payloads]:
                hidden = Path(str(artifact) + '.hidden')
                artifact.rename(hidden)
                moved.append((artifact, hidden))
            check(label + '_native_source_files_absent', not index.exists() and
                  not any(payload.exists() for payload in payloads) and
                  not list(root.glob(aa.name + '_h*')))
            actual = root / (label + '_no_native_source_all.fa')
            run(label + '_no_native_source_all', ['aastoreexportfasta', aa, parallel, actual,
                '--aastore', 1, '--threads', 4, '-v', 0])
            check(label + '_no_native_source_all_bytes', same_file(expected, actual))
            actual_rep = root / (label + '_no_native_source_rep.fa')
            run(label + '_no_native_source_rep', ['aastoreexportfasta', aa, clusters, actual_rep,
                '--aastore', 1, '--representative-only', 1, '--threads', 4, '-v', 0])
            check(label + '_no_native_source_rep_bytes', same_file(
                  root / (label + '_rep_t4_native.fa'), actual_rep))
            actual_tsv = root / (label + '_no_native_source.tsv')
            run(label + '_no_native_source_tsv', ['createtsv', aa, aa, clusters, actual_tsv,
                '--aastore', 1, '--threads', 4, '-v', 0])
            check(label + '_no_native_source_tsv_bytes', same_file(
                  root / (label + '_tsv_default_native.tsv'), actual_tsv))
        finally:
            for original, hidden in reversed(moved):
                hidden.rename(original)

        if not shuffle and not compressed:
            def reject_records(suffix, source):
                for kind, command, output in (
                        ('fasta', ['aastoreexportfasta', source, clusters], root / (suffix + '.fa')),
                        ('tsv', ['createtsv', source, source, clusters], root / (suffix + '.tsv'))):
                    run(suffix + '_' + kind, [*command, output, '--aastore', 1, '-v', 1], success=False)
                    check(suffix + '_' + kind + '_no_output', not output.exists())

            # A same-size, byte-identical replacement must still invalidate the
            # certificate's metadata-file identity, independently of mtime.
            metadata = store / 'meta.bin'
            original = store / 'meta.bin.saved'
            stamp = metadata.stat()
            metadata.rename(original)
            try:
                metadata.write_bytes(original.read_bytes())
                os.utime(metadata, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                reject_records('replaced_sequence_metadata', aa)
            finally:
                metadata.unlink()
                original.rename(metadata)
            # Exercise the timestamp check without changing inode or contents.
            try:
                os.utime(metadata, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 1000000000))
                reject_records('touched_sequence_metadata', aa)
            finally:
                os.utime(metadata, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))

            keys, subset = root / 'record_subset_keys', root / 'record_subset'
            keys.write_text('8\n2\n')
            run('record_subset_create', ['createsubdb', keys, aa, subset, '--subdb-mode', 1, '-v', 0])
            check('record_subset_has_member_view', Path(str(subset) + '.aastore.ids').is_file())
            reject_records('subset_is_not_dense_record_source', subset)
            # An old subset can share the upgraded store certificate without
            # having an explicit SUBSET tag or saved member table. It must not
            # inherit the main store's dense-ID authorization by inference.
            Path(str(subset) + '.aastore.path').write_text(str(store) + '\n')
            Path(str(subset) + '.aastore.ids').unlink()
            reject_records('legacy_subset_is_not_dense_record_source', subset)

        disabled = root / (label + '_disabled.fa')
        run(label + '_disabled', ['aastoreexportfasta', aa, clusters, disabled,
            '--aastore', 0, '-v', 1], success=False)
        check(label + '_disabled_no_output', not disabled.exists())
        marker = store / 'mmseqs_headers.lossless'
        hidden_marker = store / 'mmseqs_headers.lossless.hidden'
        marker.rename(hidden_marker)
        try:
            rejected = root / (label + '_uncertified.fa')
            run(label + '_uncertified', ['aastoreexportfasta', aa, clusters, rejected,
                '--aastore', 1, '-v', 1], success=False)
            check(label + '_uncertified_no_output', not rejected.exists())
        finally:
            hidden_marker.rename(marker)

    print('{} direct export checks passed; logs: {}'.format(len(checks), root))


if __name__ == '__main__':
    main()
