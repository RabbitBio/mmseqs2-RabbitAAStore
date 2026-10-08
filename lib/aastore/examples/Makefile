# Build this example directly from the checkout; no SDK installation is needed.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

JOBS ?= 4
CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG
IO_URING ?= AUTO
FASTA ?= data/huge_100k.fa
DATA ?= $(FASTA)
STORE ?= output/huge_100k.aastore
THREADS ?= 4

# Respect make -j when supplied; otherwise keep the default build modest.
ifeq ($(filter -j% j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(JOBS)
endif
MAKEFLAGS += --no-print-directory

EXAMPLE := aastore_example
OBJECT_DIR := .objects
LIB_SOURCES := $(wildcard ../src/*.cpp)
LIB_OBJECTS := $(patsubst ../src/%.cpp,$(OBJECT_DIR)/%.o,$(LIB_SOURCES))
OBJECTS := $(LIB_OBJECTS) $(OBJECT_DIR)/example.o
DEPENDENCIES := $(OBJECTS:.o=.d)
EXAMPLE_CPPFLAGS := -I../include -I../src
EXAMPLE_CXXFLAGS := -std=c++17 -pthread -Wall -Wextra -Wpedantic
EXAMPLE_LDLIBS := -pthread

# Compile and link a capability check without running it. Temporary probe files
# stay outside the checkout and are removed even when compilation fails.
define compile_probe
$(shell probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/aastore-example.XXXXXX") || exit 1; \
	trap 'rm -rf "$$probe_dir"' 0 1 2 3 15; \
	printf '%s\n' $(1) > "$$probe_dir/probe.cpp"; \
	if $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(EXAMPLE_CXXFLAGS) $(2) \
		"$$probe_dir/probe.cpp" $(LDFLAGS) $(LDLIBS) $(3) \
		-o "$$probe_dir/probe" >/dev/null 2>&1; then printf '%s' yes; fi)
endef

# make clean/help must also work without a compiler or optional dependencies.
NEED_PROBES := $(if $(MAKECMDGOALS),$(filter-out clean help,$(MAKECMDGOALS)),all)
ifneq ($(strip $(NEED_PROBES)),)
FILESYSTEM_PROBE := '\#include <filesystem>' \
	'int main() { return std::filesystem::exists(std::filesystem::current_path()) ? 0 : 1; }'
ifneq ($(call compile_probe,$(FILESYSTEM_PROBE)),yes)
ifeq ($(call compile_probe,$(FILESYSTEM_PROBE),,-lstdc++fs),yes)
EXAMPLE_LDLIBS += -lstdc++fs
else ifeq ($(call compile_probe,$(FILESYSTEM_PROBE),,-lc++fs),yes)
EXAMPLE_LDLIBS += -lc++fs
else
$(error The selected C++17 compiler cannot link std::filesystem)
endif
endif

ifneq ($(filter $(IO_URING),AUTO ON),)
URING_CFLAGS ?= $(shell pkg-config --cflags liburing 2>/dev/null)
URING_LIBS ?= $(shell pkg-config --libs liburing 2>/dev/null || printf '%s' '-luring')
URING_BASELINE_PROBE := '\#include <liburing.h>' '\#include <sys/uio.h>' \
	'int main() { io_uring ring = {}; io_uring_params params = {}; io_uring_queue_init_params(1, &ring, &params); io_uring_sqe* sqe = io_uring_get_sqe(&ring); iovec buffer = {}; io_uring_prep_readv(sqe, -1, &buffer, 1, 0); sqe->user_data = 0; io_uring_submit(&ring); io_uring_cqe* cqe = nullptr; io_uring_peek_cqe(&ring, &cqe); io_uring_wait_cqe(&ring, &cqe); (void)cqe->res; (void)cqe->user_data; (void)io_uring_cq_ready(&ring); io_uring_cqe_seen(&ring, cqe); io_uring_queue_exit(&ring); return 0; }'
URING_AVAILABLE := $(call compile_probe,$(URING_BASELINE_PROBE),$(URING_CFLAGS),$(URING_LIBS))
ifneq ($(URING_AVAILABLE),yes)
ifeq ($(IO_URING),ON)
$(error IO_URING=ON requires usable liburing headers and library; use IO_URING=AUTO to allow pread fallback)
endif
endif
ifeq ($(URING_AVAILABLE),yes)
EXAMPLE_CPPFLAGS += $(URING_CFLAGS) -DAASTORE_HAVE_IO_URING=1
EXAMPLE_LDLIBS += $(URING_LIBS)
URING_READ_PROBE := '\#include <liburing.h>' \
	'int main() { io_uring_sqe sqe = {}; char buffer = 0; io_uring_prep_read(&sqe, -1, &buffer, 1, 0); return sqe.opcode == IORING_OP_READ ? 0 : 1; }'
ifeq ($(call compile_probe,$(URING_READ_PROBE),$(URING_CFLAGS),$(URING_LIBS)),yes)
EXAMPLE_CPPFLAGS += -DAASTORE_URING_HAS_READ=1
endif
URING_PROBE_PROBE := '\#include <cstdlib>' '\#include <liburing.h>' \
	'int main() { io_uring ring = {}; io_uring_probe* probe = io_uring_get_probe_ring(&ring); const int supported = probe ? io_uring_opcode_supported(probe, IORING_OP_READV) : 0; std::free(probe); return supported; }'
ifeq ($(call compile_probe,$(URING_PROBE_PROBE),$(URING_CFLAGS),$(URING_LIBS)),yes)
EXAMPLE_CPPFLAGS += -DAASTORE_URING_HAS_PROBE=1
endif
endif
else ifneq ($(IO_URING),OFF)
$(error IO_URING must be AUTO, ON, or OFF)
endif
endif

.PHONY: all build run clean help FORCE

all: $(EXAMPLE)
build: all

$(EXAMPLE): $(OBJECTS) $(OBJECT_DIR)/build_flags
	$(CXX) $(CXXFLAGS) $(EXAMPLE_CXXFLAGS) $(LDFLAGS) $(OBJECTS) $(LDLIBS) $(EXAMPLE_LDLIBS) -o $@

$(OBJECT_DIR)/%.o: ../src/%.cpp Makefile $(OBJECT_DIR)/build_flags | $(OBJECT_DIR)
	$(CXX) $(CPPFLAGS) $(EXAMPLE_CPPFLAGS) $(CXXFLAGS) $(EXAMPLE_CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJECT_DIR)/example.o: cpp/main.cpp Makefile $(OBJECT_DIR)/build_flags | $(OBJECT_DIR)
	$(CXX) $(CPPFLAGS) $(EXAMPLE_CPPFLAGS) $(CXXFLAGS) $(EXAMPLE_CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJECT_DIR):
	mkdir -p $@

# Recompile after changing compilers, flags, or IO_URING; leave the timestamp
# untouched when settings match so repeated make calls remain incremental.
$(OBJECT_DIR)/build_flags: FORCE | $(OBJECT_DIR)
	@printf '%s\n' '$(CXX)' '$(CPPFLAGS) $(EXAMPLE_CPPFLAGS)' \
		'$(CXXFLAGS) $(EXAMPLE_CXXFLAGS)' '$(LDFLAGS)' \
		'$(LDLIBS) $(EXAMPLE_LDLIBS)' '$(LIB_SOURCES)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

run: $(EXAMPLE)
	@mkdir -p "$(dir $(STORE))"
	./$(EXAMPLE) "$(DATA)" "$(STORE)" "$(THREADS)"

# Preserve the bundled FASTA and sources, and never remove an overridden STORE.
clean:
	rm -rf -- $(OBJECT_DIR) $(EXAMPLE) output/huge_100k.aastore

help:
	@printf '%s\n' \
		'Run inside examples/:' \
		'  make                  Compile ./aastore_example from the local sources' \
		'  make run              Build, then run on data/huge_100k.fa' \
		'  make clean            Remove generated objects, executable, and the default store' \
		'Options: JOBS=4 THREADS=4 CXX=g++ IO_URING=AUTO' \
		'         DATA=data/huge_100k.fa STORE=output/huge_100k.aastore' \
		'         IO_URING=AUTO detects liburing; ON requires it; OFF disables it' \
		'         URING_CFLAGS="..." URING_LIBS="..."'

-include $(DEPENDENCIES)
