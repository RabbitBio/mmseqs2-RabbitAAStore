# Configure and build the full regression suite without a second compiler setup.
.DEFAULT_GOAL := build

SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR ?= $(SOURCE_DIR)/tests/build
BUILD_TYPE ?= Release
JOBS ?= 4
CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= OFF
IO_URING ?= ON
BENCHMARKS ?= ON
CMAKE_ARGS ?=
CTEST_ARGS ?=

.PHONY: all configure build test clean help

all: build

configure:
	"$(CMAKE)" -S "$(SOURCE_DIR)" -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$$<1:$(SOURCE_DIR)/tests>" \
		-DAASTORE_BUILD_TESTS=ON -DAASTORE_RUN_SMOKE_TESTS=OFF \
		-DAASTORE_BUILD_TOOLS=ON -DAASTORE_BUILD_BENCHMARKS=$(BENCHMARKS) \
		-DAASTORE_BUILD_PYTHON=$(PYTHON) -DAASTORE_ENABLE_IO_URING=$(IO_URING) \
		$(CMAKE_ARGS)

build: configure
	+"$(CMAKE)" --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --parallel "$(JOBS)"

test: build
	cd "$(BUILD_DIR)" && "$(CTEST)" -C "$(BUILD_TYPE)" --output-on-failure $(CTEST_ARGS)

# Only remove generated build outputs; retain configuration and test reports.
clean:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		"$(CMAKE)" --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --target clean; \
	fi

help:
	@printf '%s\n' \
		'Run inside tests/:' \
		'  make                                Compile executables into tests/' \
		'  make test                           Compile, then run all registered tests' \
		'  make test CTEST_ARGS="-L sequential" Select a test label' \
		'  make clean                          Clean compiled files, keeping the cache' \
		'Options: JOBS=4 BUILD_TYPE=Release BUILD_DIR=/path/to/test-build' \
		'         PYTHON=ON IO_URING=OFF BENCHMARKS=OFF CMAKE_ARGS="..." CTEST_ARGS="..."'
