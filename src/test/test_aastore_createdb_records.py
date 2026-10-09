"""Check header-free AAStore construction and complete cluster workflows."""
import argparse
import gzip
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from test_aastore_exports import (read_database, write_clusters, text_column,
                                  same_file, replace_headers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--builder', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    binary, root = args.mmseqs.resolve(), args.workdir.resolve()
    builder = args.builder.resolve()
    root.mkdir(parents=True, exist_ok=False)
    checks = []

    def run(label, arguments, stdin=None, success=True, executable=None):
        log = root / (label + '.log')
        with log.open('wb') as stream:
            result = subprocess.run([str(executable or binary), *map(str, arguments)],
                input=stdin, stdout=stream, stderr=subprocess.STDOUT, timeout=300)
        if (result.returncode == 0) != success:
            raise AssertionError('{}: exit {}; {}'.format(label, result.returncode, log))
        return log.read_text()

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        checks.append(label)
        (root / 'checks.json').write_text(json.dumps(checks, indent=2))
        print(label + ': passed', flush=True)

    fasta = root / 'input.fa'
    alphabet = 'ACDEFGHIKLMNPQRSTVWY'
    fasta.write_text(''.join('>sp|P{:05d}|ENTRY{}\t\tdesc {}  \n{}\n'.format(
        i, i, i, alphabet * (i % 5 + 1)) for i in range(4202)))
    compressed_fasta = root / 'input.fa.gz'
    compressed_fasta.write_bytes(gzip.compress(fasta.read_bytes()))
    cases = [(0, 0, 0, fasta), (0, 1, 0, fasta), (0, 1, 1, fasta),
             (1, 0, 0, fasta), (1, 1, 0, fasta), (1, 1, 0, compressed_fasta)]
    for number, (mode, shuffle, compressed, source) in enumerate(cases):
        label = 'case{}'.format(number)
        native, aa = root / (label + '_native'), root / (label + '_aa')
        common = ['--shuffle', shuffle, '--createdb-mode', mode,
                  '--compressed', compressed, '--dbtype', 1, '--threads', 4, '-v', 3]
        for database, enabled in ((native, 0), (aa, 1)):
            log = run(label + '_createdb_' + str(enabled),
                      ['createdb', source, database, '--aastore', enabled, *common])
            if enabled:
                check(label + '_build_records_log', 'native_header_db=0' in log)
        check(label + '_no_native_header_files', not list(root.glob(aa.name + '_h*')))
        check(label + '_no_staging', not list(root.glob(aa.name + '.aastore.text.tmp.*')))
        store = Path(str(aa) + '.aastore')
        names, descriptions = text_column(store, 'names'), text_column(store, 'descriptions')
        ordered_fasta, default_store = root / (label + '_ordered.fa'), root / (label + '_default_store')
        run(label + '_native_export', ['convert2fasta', native, ordered_fasta, '-v', 0])
        run(label + '_default_build', [ordered_fasta, default_store, '--threads', 1], executable=builder)
        for name in ('packed.bin', 'meta.bin', 'names.bin', 'names.meta', 'descriptions.bin', 'descriptions.meta'):
            check(label + '_default_' + name, same_file(store / name, default_store / name))
        check(label + '_lookup_matches', Path(str(native) + '.lookup').read_bytes() == Path(str(aa) + '.lookup').read_bytes())
        check(label + '_index_matches', Path(str(native) + '.index').read_bytes() == Path(str(aa) + '.index').read_bytes())
        # The default AAStore representation normalizes the name/description
        # separator. Format native oracle headers the same way; keep the
        # original sequence payload and ID order for independent exports.
        header_path = Path(str(native) + '_h')
        if header_path.is_symlink():
            header_path.unlink()
        replace_headers(native, {key: name + (b' ' if descriptions[key] else b'') + descriptions[key]
                                 for key, name in enumerate(names)})
        clusters = root / (label + '_clusters')
        write_clusters(clusters, [(9, [2, 1]), (0, [0, 137, 0]), (4096, [4096, 4199])])
        body, expected_all, actual_all = (root / (label + suffix) for suffix in ('_body', '_expected.fa', '_all.fa'))
        run(label + '_native_body', ['createseqfiledb', native, clusters, body, '--threads', 1, '-v', 0])
        run(label + '_native_flat', ['result2flat', native, native, body, expected_all, '-v', 0])
        log = run(label + '_direct_all', ['aastoreexportfasta', aa, clusters, actual_all, '--aastore', 1, '--threads', 4, '-v', 3])
        check(label + '_all_bytewise', same_file(expected_all, actual_all))
        check(label + '_all_records_backend', 'batch_get_records fields=all header=AAStore' in log)
        rep_body, expected_rep, actual_rep = (root / (label + suffix) for suffix in ('_rep_body', '_expected_rep.fa', '_rep.fa'))
        run(label + '_native_rep', ['result2repseq', native, clusters, rep_body, '--threads', 1, '-v', 0])
        run(label + '_native_rep_flat', ['result2flat', native, native, rep_body, expected_rep, '--use-fasta-header', 1, '-v', 0])
        run(label + '_direct_rep', ['aastoreexportfasta', aa, clusters, actual_rep, '--aastore', 1, '--representative-only', 1, '--threads', 4, '-v', 3])
        check(label + '_rep_bytewise', same_file(expected_rep, actual_rep))
        for suffix, options in [('tsv', []), ('tsv_full', ['--full-header', 1]), ('tsv_first', ['--first-seq-as-repr', 1]),
                                ('tsv_numeric', ['--target-column', 0])]:
            expected_tsv, actual_tsv = root / (label + '_' + suffix + '_native'), root / (label + '_' + suffix + '_aa')
            run(label + '_' + suffix + '_native', ['createtsv', native, native, clusters, expected_tsv, '--threads', 1, '-v', 0, *options])
            log = run(label + '_' + suffix + '_aa', ['createtsv', aa, aa, clusters, actual_tsv, '--aastore', 1, '--threads', 4, '-v', 3, *options])
            check(label + '_' + suffix + '_bytewise', same_file(expected_tsv, actual_tsv))
            check(label + '_' + suffix + '_text_backend', 'fields=name,description native_header_db=0' in log)

        if number == 4:
            # Soft-createdb payload links are not an export dependency either.
            # Move the links themselves, retaining the separate native oracle.
            index = Path(str(aa) + '.index')
            payloads = ([aa] if aa.is_file() else sorted(
                item for item in aa.parent.glob(aa.name + '.*') if item.suffix[1:].isdigit()))
            hidden = []
            try:
                for artifact in [index, *payloads]:
                    destination = Path(str(artifact) + '.hidden')
                    artifact.rename(destination)
                    hidden.append((artifact, destination))
                check(label + '_native_source_absent', not index.exists() and
                      bool(payloads) and not any(path.exists() for path in payloads) and
                      not list(root.glob(aa.name + '_h*')))
                all_output, rep_output, tsv_output = (
                    root / (label + '_independent' + extension)
                    for extension in ('_all.fa', '_rep.fa', '.tsv'))
                run(label + '_independent_all', ['aastoreexportfasta', aa, clusters,
                    all_output, '--aastore', 1, '--threads', 4, '-v', 0])
                check(label + '_independent_all_bytes', same_file(expected_all, all_output))
                run(label + '_independent_rep', ['aastoreexportfasta', aa, clusters,
                    rep_output, '--aastore', 1, '--representative-only', 1, '--threads', 4, '-v', 0])
                check(label + '_independent_rep_bytes', same_file(expected_rep, rep_output))
                run(label + '_independent_tsv', ['createtsv', aa, aa, clusters,
                    tsv_output, '--aastore', 1, '--threads', 4, '-v', 0])
                check(label + '_independent_tsv_bytes', same_file(root / (label + '_tsv_native'), tsv_output))
            finally:
                for artifact, destination in reversed(hidden):
                    destination.rename(artifact)

        if number == 0:
            # Query labels come from the first target token verbatim when no
            # target database was provided, even when it contains leading zeroes.
            numeric = root / 'tsv_numeric_tokens'
            write_clusters(numeric, [(9, ['0002', '0001']), (0, ['0137', '0000']), (4, [])])
            for suffix, options in [('default', []), ('first', ['--first-seq-as-repr', 1]),
                                    ('first_no_target_column', ['--first-seq-as-repr', 1, '--target-column', 0]),
                                    ('full', ['--full-header', 1])]:
                expected, actual = root / ('numeric_' + suffix + '_native.tsv'), root / ('numeric_' + suffix + '_aa.tsv')
                run('numeric_' + suffix + '_native', ['createtsv', native, numeric, expected, '--threads', 1, '-v', 0, *options])
                run('numeric_' + suffix + '_aa', ['createtsv', aa, numeric, actual, '--aastore', 1, '--threads', 4, '-v', 0, *options])
                check('numeric_' + suffix + '_bytewise', same_file(expected, actual))
                if suffix == 'first':
                    check('numeric_first_keeps_leading_zeroes', b'0002\t0001\n' in actual.read_bytes())

            # Replace a later numeric column and retain all other columns.
            columns = root / 'tsv_later_column'
            write_clusters(columns, [(9, ['labelA\t0002\t7', 'labelB\t0001\t8']), (4, [])])
            for first_repr in (0, 1):
                for full_header in (0, 1):
                    suffix = 'later_first{}_full{}'.format(first_repr, full_header)
                    options = ['--target-column', 2, '--first-seq-as-repr', first_repr, '--full-header', full_header]
                    expected, actual = root / (suffix + '_native.tsv'), root / (suffix + '_aa.tsv')
                    run(suffix + '_native', ['createtsv', native, native, columns, expected, '--threads', 1, '-v', 0, *options])
                    run(suffix + '_aa', ['createtsv', aa, aa, columns, actual, '--aastore', 1, '--threads', 4, '-v', 0, *options])
                    check(suffix + '_bytewise', same_file(expected, actual))

            compressed_clusters = root / 'tsv_output_clusters'
            write_clusters(compressed_clusters, [(9, [2] * 50), (0, [0, 137]), (4, [])])
            results = []
            for source_db, enabled in ((native, 0), (aa, 1)):
                output = root / ('tsv_compressed_' + str(enabled))
                unpacked = root / ('tsv_unpacked_' + str(enabled))
                run('tsv_compressed_' + str(enabled), ['createtsv', source_db, source_db, compressed_clusters, output,
                    '--aastore', enabled, '--threads', 1, '--db-output', 1, '--compressed', 1, '-v', 0])
                run('tsv_decompress_' + str(enabled), ['decompress', output, unpacked, '--threads', 1, '-v', 0])
                results.append(read_database(unpacked))
            check('tsv_compressed_record_boundaries', results[0] == results[1])

            empty_rep, native_rep, actual_rep = root / 'empty_rep_body', root / 'empty_rep_native.fa', root / 'empty_rep_aa.fa'
            run('empty_rep_native_body', ['result2repseq', native, numeric, empty_rep, '--threads', 1, '-v', 0])
            run('empty_rep_native_flat', ['result2flat', native, native, empty_rep, native_rep, '--use-fasta-header', 1, '-v', 0])
            run('empty_rep_aa', ['aastoreexportfasta', aa, numeric, actual_rep, '--aastore', 1, '--representative-only', 1,
                '--threads', 4, '-v', 0])
            check('representative_empty_cluster_skipped', same_file(native_rep, actual_rep))

    # The easy entrypoints must work without any intermediate native header DB
    # or ASCII output database, including their default cleanup.
    small = root / 'workflow.fa'
    small.write_text('>one first\n' + alphabet * 4 + '\n>two\tdescription\n' + alphabet * 4 + '\n>three\nWQMKHINPSTWQMKHINPST\n')
    for module in ('easy-linclust', 'easy-cluster'):
        output, temporary = root / (module + '_result'), root / (module + '_tmp')
        log = run(module, [module, small, output, temporary, '--aastore', 1,
            '--min-seq-id', 0.9, '-c', 0.8, '--threads', 4, '--remove-tmp-files', 0, '-v', 3])
        directory = (temporary / 'latest').resolve()
        check(module + '_no_header_db', not list(directory.glob('input_h*')))
        check(module + '_no_ascii_intermediates', not list(directory.glob('clu_rep*')) and not list(directory.glob('clu_seqs*')))
        check(module + '_two_direct_exports', log.count('AAStore aastoreexportfasta: batch_get_records fields=all') == 2)
        for suffix in ('_cluster.tsv', '_rep_seq.fasta', '_all_seqs.fasta'):
            check(module + suffix, Path(str(output) + suffix).stat().st_size > 0)
    cleanup_output, cleanup_tmp = root / 'cleanup_result', root / 'cleanup_tmp'
    run('cleanup', ['easy-linclust', small, cleanup_output, cleanup_tmp, '--aastore', 1,
                   '--threads', 4, '--remove-tmp-files', 1, '-v', 0])
    check('cleanup_store_removed', not list((cleanup_tmp / 'latest').resolve().glob('input.aastore*')))
    rebuilt = root / 'rebuilt'
    run('rebuild_native', ['createdb', fasta, rebuilt, '--dbtype', 1, '-v', 0])
    check('rebuild_native_has_headers', Path(str(rebuilt) + '_h.dbtype').exists())
    run('rebuild_records', ['createdb', small, rebuilt, '--aastore', 1, '--dbtype', 1, '-v', 0])
    check('rebuild_removes_old_headers', not list(root.glob('rebuilt_h*')))
    check('rebuild_refreshes_names', sorted(text_column(Path(str(rebuilt) + '.aastore'), 'names')) ==
          [b'one', b'three', b'two'])
    print('{} automatic records checks passed; logs: {}'.format(len(checks), root))


if __name__ == '__main__':
    main()
