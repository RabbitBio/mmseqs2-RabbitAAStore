# 文件职责：描述公开 Python 接口的类型，供 IDE 和静态检查器使用。
from enum import Enum
from os import PathLike
from typing import Any, Dict, Iterable, Iterator, List, Optional, Union, overload

Path = Union[str, PathLike[str]]
__version__: str

class StorageMode(Enum):
    ByteRange = 0
    PrefixMemory = 1
    FullMemory = 3

class IoBackend(Enum):
    Auto = 0
    Pread = 1
    IoUring = 2

class ReadStrategy(Enum):
    Sparse = 0
    Scan = 1

class RecordFields(Enum):
    None_ = 0
    Name = 1
    Description = 2
    Sequence = 4
    All = 7
    def __or__(self, other: RecordFields) -> RecordFields: ...
    def __and__(self, other: RecordFields) -> RecordFields: ...

class BuildOptions:
    threads: int
    overwrite: bool
    write_names: bool
    write_descriptions: bool
    def __init__(self, *, threads: int = 1, overwrite: bool = False,
                 write_names: bool = True, write_descriptions: bool = True) -> None: ...

class StoreOptions:
    storage_mode: StorageMode
    cache_bytes: int
    threads: int
    resident_names: bool
    resident_descriptions: bool
    def __init__(self, *, storage_mode: StorageMode = StorageMode.ByteRange,
                 cache_bytes: int = 0, threads: int = 8,
                 resident_names: bool = False, resident_descriptions: bool = False) -> None: ...

class ReadOptions:
    threads: int
    io_backend: IoBackend
    io_queue_depth: int
    io_buffer_bytes: int
    input_sorted: bool
    read_strategy: ReadStrategy
    merge_gap_bytes: int
    max_merged_range_bytes: int
    max_exact_merged_range_bytes: int
    scan_max_range_bytes: int
    collect_diagnostics: bool
    def __init__(self, *, threads: int = 8, io_backend: IoBackend = IoBackend.Auto,
                 io_queue_depth: int = 64, io_buffer_bytes: int = 16777216,
                 input_sorted: bool = False, read_strategy: ReadStrategy = ReadStrategy.Sparse,
                 merge_gap_bytes: int = 4096, max_merged_range_bytes: int = 16384,
                 max_exact_merged_range_bytes: int = 1048576, scan_max_range_bytes: int = 262144,
                 collect_diagnostics: bool = False) -> None: ...

class BuildStats:
    sequences: int
    residues: int
    packed_bytes: int
    name_bytes: int
    description_bytes: int

class BatchStats:
    full_memory: bool
    prefix_memory: bool
    io_uring: bool
    requested_sequences: int
    requested_residues: int
    cache_hits: int
    cache_misses: int
    cache_hit_residues: int
    cache_miss_residues: int
    unique_packed_bytes: int
    ranges: int
    sparse_ranges: int
    scan_ranges: int
    merged_gaps: int
    free_merges: int
    read_bytes: int
    overread_bytes: int
    max_range_bytes: int
    workers: int
    io_workers: int
    decode_workers: int
    io_queue_depth: int
    io_submissions: int
    io_completions: int
    io_submit_calls: int
    io_submit_ns: int
    io_wait_calls: int
    io_empty_waits: int
    layout_ns: int
    sort_ns: int
    range_plan_ns: int
    worker_wall_ns: int
    worker_max_busy_ns: int
    read_ns: int
    decode_ns: int
    cache_decode_ns: int
    diagnostics_collected: bool

class TextBatchStats:
    resident: bool
    io_uring: bool
    requested_strings: int
    requested_bytes: int
    unique_bytes: int
    ranges: int
    merged_gaps: int
    read_bytes: int
    overread_bytes: int
    max_range_bytes: int

class Batch:
    @property
    def stats(self) -> BatchStats: ...
    @property
    def storage(self) -> memoryview: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[str]: ...
    @overload
    def __getitem__(self, index: int) -> str: ...
    @overload
    def __getitem__(self, index: slice) -> List[str]: ...
    def size(self) -> int: ...
    def empty(self) -> bool: ...
    def view(self, index: int) -> memoryview: ...

class TextBatch:
    @property
    def stats(self) -> TextBatchStats: ...
    @property
    def storage(self) -> memoryview: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[str]: ...
    @overload
    def __getitem__(self, index: int) -> str: ...
    @overload
    def __getitem__(self, index: slice) -> List[str]: ...
    def size(self) -> int: ...
    def empty(self) -> bool: ...
    def view(self, index: int) -> memoryview: ...

class RecordBatch:
    @property
    def fields(self) -> RecordFields: ...
    @property
    def names(self) -> TextBatch: ...
    @property
    def descriptions(self) -> TextBatch: ...
    @property
    def sequences(self) -> Batch: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Dict[str, str]]: ...
    @overload
    def __getitem__(self, index: int) -> Dict[str, str]: ...
    @overload
    def __getitem__(self, index: slice) -> List[Dict[str, str]]: ...
    def size(self) -> int: ...
    def empty(self) -> bool: ...
    def name(self, index: int) -> str: ...
    def description(self, index: int) -> str: ...
    def sequence(self, index: int) -> str: ...

class Store:
    def __init__(self, path: Path, options: StoreOptions = ...) -> None: ...
    def __enter__(self) -> Store: ...
    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None: ...
    def __len__(self) -> int: ...
    def __getitem__(self, index: int) -> str: ...
    def size(self) -> int: ...
    def close(self) -> None: ...
    @property
    def closed(self) -> bool: ...
    @property
    def total_residues(self) -> int: ...
    @property
    def packed_bytes(self) -> int: ...
    @property
    def storage_mode(self) -> StorageMode: ...
    @property
    def resident_bytes(self) -> int: ...
    @property
    def resident_text_bytes(self) -> int: ...
    @property
    def preload_ns(self) -> int: ...
    @property
    def has_names(self) -> bool: ...
    @property
    def has_descriptions(self) -> bool: ...
    @property
    def decode_kernel_name(self) -> str: ...
    def sequence_length(self, id: int) -> int: ...
    def get(self, id: int) -> str: ...
    def batch_get(self, ids: Iterable[int], options: ReadOptions = ...) -> Batch: ...
    def batch_get_into(self, ids: Iterable[int], outputs: Iterable[Any],
                       options: ReadOptions = ...) -> BatchStats: ...
    def sequential_get(self, begin: int, end: int, options: ReadOptions = ...) -> Batch: ...
    def sequential_get_into(self, begin: int, end: int, outputs: Iterable[Any],
                            options: ReadOptions = ...) -> BatchStats: ...
    def batch_get_records(self, ids: Iterable[int], fields: RecordFields = RecordFields.All,
                          options: ReadOptions = ...) -> RecordBatch: ...

def build_fasta(fasta: Path, store: Path, options: Optional[BuildOptions] = None, *,
                threads: Optional[int] = None, overwrite: Optional[bool] = None,
                write_names: Optional[bool] = None, write_descriptions: Optional[bool] = None
                ) -> BuildStats: ...
