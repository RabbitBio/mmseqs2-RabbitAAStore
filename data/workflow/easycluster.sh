#!/bin/sh -e
fail() {
    echo "Error: $1"
    exit 1
}

notExists() {
   [ ! -f "$1" ]
}


if notExists "${TMP_PATH}/input.dbtype"; then
    # shellcheck disable=SC2086
    "$MMSEQS" createdb "$@" "${TMP_PATH}/input" ${CREATEDB_PAR} \
        || fail "query createdb died"
fi

if notExists "${TMP_PATH}/clu.dbtype"; then
    # shellcheck disable=SC2086
    "$MMSEQS" "${CLUSTER_MODULE}" "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/clu_tmp" ${CLUSTER_PAR} \
        || fail "Search died"
fi

if notExists "${TMP_PATH}/cluster.tsv"; then
    TSV_PAR="${THREADS_PAR}"
    if [ -n "${AASTORE}" ]; then
        TSV_PAR="${TSV_PAR} --aastore 1"
    fi
    # shellcheck disable=SC2086
    "$MMSEQS" createtsv "${TMP_PATH}/input" "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/cluster.tsv" ${TSV_PAR} \
        || fail "Convert Alignments died"
fi

if notExists "${TMP_PATH}/rep_seq.fasta"; then
    if [ -n "${AASTORE}" ]; then
        # shellcheck disable=SC2086
        "$MMSEQS" aastoreexportfasta "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/rep_seq.fasta" ${AASTORE_REP_EXPORT_PAR} \
            || fail "AAStore representative export died"
    else
        # shellcheck disable=SC2086
        "$MMSEQS" result2repseq "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/clu_rep" ${RESULT2REPSEQ_PAR} \
            || fail "Result2repseq  died"

        # shellcheck disable=SC2086
        "$MMSEQS" result2flat "${TMP_PATH}/input" "${TMP_PATH}/input" "${TMP_PATH}/clu_rep" "${TMP_PATH}/rep_seq.fasta" --use-fasta-header ${VERBOSITY_PAR} \
            || fail "result2flat died"
    fi
fi

if notExists "${TMP_PATH}/all_seqs.fasta"; then
    if [ -n "${AASTORE}" ]; then
        # shellcheck disable=SC2086
        "$MMSEQS" aastoreexportfasta "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/all_seqs.fasta" ${AASTORE_EXPORT_PAR} \
            || fail "AAStore member export died"
    else
        # shellcheck disable=SC2086
        "$MMSEQS" createseqfiledb "${TMP_PATH}/input" "${TMP_PATH}/clu" "${TMP_PATH}/clu_seqs" ${THREADS_PAR} \
            || fail "Result2repseq  died"

        # shellcheck disable=SC2086
        "$MMSEQS" result2flat "${TMP_PATH}/input" "${TMP_PATH}/input" "${TMP_PATH}/clu_seqs" "${TMP_PATH}/all_seqs.fasta" ${VERBOSITY_PAR} \
            || fail "result2flat died"
    fi
fi

mv "${TMP_PATH}/all_seqs.fasta"  "${RESULTS}_all_seqs.fasta"
mv "${TMP_PATH}/rep_seq.fasta"  "${RESULTS}_rep_seq.fasta"
mv "${TMP_PATH}/cluster.tsv"  "${RESULTS}_cluster.tsv"

if [ -n "${REMOVE_TMP}" ]; then
    if [ -n "${AASTORE}" ]; then
        rm -rf -- "${TMP_PATH}/input.aastore"
        rm -f -- "${TMP_PATH}/input.aastore.path"
    fi
    # shellcheck disable=SC2086
    "$MMSEQS" rmdb "${TMP_PATH}/input" ${VERBOSITY_PAR}
    # shellcheck disable=SC2086
    if [ -f "${TMP_PATH}/input_h.dbtype" ]; then
        "$MMSEQS" rmdb "${TMP_PATH}/input_h" ${VERBOSITY_PAR}
    fi
    # shellcheck disable=SC2086
    if [ -f "${TMP_PATH}/clu_seqs.dbtype" ]; then
        "$MMSEQS" rmdb "${TMP_PATH}/clu_seqs" ${VERBOSITY_PAR}
    fi
    # shellcheck disable=SC2086
    if [ -f "${TMP_PATH}/clu_rep.dbtype" ]; then
        "$MMSEQS" rmdb "${TMP_PATH}/clu_rep" ${VERBOSITY_PAR}
    fi
    # shellcheck disable=SC2086
    "$MMSEQS" rmdb "${TMP_PATH}/clu" ${VERBOSITY_PAR}
    rm -rf "${TMP_PATH}/clu_tmp"
    rm -f "${TMP_PATH}/easycluster.sh"
fi
