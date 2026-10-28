"""Check RabbitAAStore index loading modes and native workflow parameter forwarding."""
import argparse
import json
from pathlib import Path
import random
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mmseqs', required=True, type=Path)
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    binary, root = args.mmseqs.resolve(), args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    rng = random.Random(17)
    alphabet = 'ACDEFGHIKLMNPQRSTVWY'
    records = []
    for family in range(6):
        sequence = ''.join(rng.choice(alphabet) for _ in range(160))
        for variant in range(6):
            changed = list(sequence)
            for position in range(variant):
                changed[position * 13] = rng.choice(alphabet)
            records.append('>seq{}_{} description {}\n{}\n'.format(
                family, variant, variant, ''.join(changed)))
    fasta = root / 'input.fa'
    fasta.write_text(''.join(records))
    checks = []
    expected = None
    for label, selected in [('default', None), ('mmap', 'mmap'), ('read', 'read')]:
        prefix = root / label
        command = [str(binary), 'easy-linclust', str(fasta), str(prefix),
                   str(root / (label + '_tmp')), '--aastore', '1',
                   '--cluster-mode', '3', '--threads', '1',
                   '--createdb-mode', '0', '-v', '3']
        if selected:
            command += ['--aastore-index-mode', selected]
        log_path = root / (label + '.log')
        with log_path.open('wb') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=300)
        if result.returncode:
            raise AssertionError('{} failed: {}'.format(label, log_path))
        log = log_path.read_text()
        mode = selected or 'mmap'
        opened_modes = re.findall(r'index_load_mode=(mmap|read)\b', log)
        if not opened_modes or any(value != mode for value in opened_modes):
            raise AssertionError('{} did not consistently open {} indexes'.format(label, mode))
        if selected:
            for child in ['createdb', 'linclust', 'rescorediagonal', 'createsubdb',
                          'align', 'createtsv', 'aastoreexportfasta']:
                lines = [line for line in log.splitlines() if line.startswith(child + ' ')]
                if not lines or any('--aastore-index-mode ' + mode not in line for line in lines):
                    raise AssertionError('{} mode not forwarded to {}'.format(label, child))
        outputs = {suffix: Path(str(prefix) + suffix).read_bytes()
                   for suffix in ['_cluster.tsv', '_rep_seq.fasta', '_all_seqs.fasta']}
        if expected is None:
            expected = outputs
        elif outputs != expected:
            raise AssertionError('{} changed clustering or FASTA output'.format(label))
        checks.append(label + ': passed')
        print(checks[-1], flush=True)
    invalid_log = root / 'invalid.log'
    with invalid_log.open('wb') as stream:
        result = subprocess.run([str(binary), 'createdb', str(fasta), str(root / 'invalid'),
                                 '--aastore', '1', '--aastore-index-mode', 'automatic'],
                                stdout=stream, stderr=subprocess.STDOUT, timeout=30)
    if result.returncode == 0 or '--aastore-index-mode must be mmap or read' not in invalid_log.read_text():
        raise AssertionError('invalid index mode was not rejected')
    if Path(str(root / 'invalid') + '.dbtype').exists():
        raise AssertionError('invalid mode created a database')
    checks.append('invalid: passed')
    (root / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
    print(checks[-1], flush=True)


if __name__ == '__main__':
    main()
