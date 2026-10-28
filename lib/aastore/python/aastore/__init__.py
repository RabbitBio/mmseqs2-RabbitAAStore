# 文件职责：导出 RabbitAAStore 的 Python 接口，并将建库关键字参数转交给 C++ 核心。
"""Compact FASTA storage backed by the RabbitAAStore C++ library."""

from ._aastore import (
    Batch,
    BatchStats,
    BuildOptions,
    BuildStats,
    IndexLoadMode,
    IoBackend,
    ReadOptions,
    RecordBatch,
    RecordFields,
    StorageMode,
    Store,
    StoreOptions,
    TextBatch,
    TextBatchStats,
    __version__,
    _build_fasta,
)

__all__ = [
    "Batch", "BatchStats", "BuildOptions", "BuildStats", "IndexLoadMode", "IoBackend",
    "ReadOptions", "RecordBatch", "RecordFields", "StorageMode",
    "Store", "StoreOptions", "TextBatch", "TextBatchStats", "build_fasta",
    "__version__",
]


# 复制调用方选项后应用显式覆盖值，不修改可复用的 BuildOptions 对象。
def build_fasta(fasta, store, options=None, *, threads=None, overwrite=None,
                write_names=None, write_descriptions=None):
    """Build a store in input order; optional keywords override BuildOptions."""
    if options is not None and not isinstance(options, BuildOptions):
        raise TypeError("options must be BuildOptions or None")
    base = options if options is not None else BuildOptions()
    configured = BuildOptions(
        threads=base.threads if threads is None else threads,
        overwrite=base.overwrite if overwrite is None else overwrite,
        write_names=base.write_names if write_names is None else write_names,
        write_descriptions=(base.write_descriptions
                            if write_descriptions is None else write_descriptions),
    )
    return _build_fasta(fasta, store, configured)
