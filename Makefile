NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
J ?= $(shell n=$(NPROC); h=$$((n / 2)); [ $$h -lt 2 ] && h=2; echo $$h)
BUILD ?= build
CMAKE_FLAGS ?=
CLANG_FORMAT ?= uvx clang-format@23.1.3
SOURCES := $(shell find include src tools tests -name '*.cc' -o -name '*.h')

.PHONY: build test crashtest sanitize bench lint format clean

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release $(CMAKE_FLAGS)
	cmake --build $(BUILD) -j$(J)

test: build
	ctest --test-dir $(BUILD) --output-on-failure -j$(J)

crashtest: build
	./$(BUILD)/sediment-crashtest --rounds 30 --dir /tmp/sediment-crashtest

sanitize:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEDIMENT_SANITIZE=address,undefined -DSEDIMENT_BUILD_BENCH=OFF
	cmake --build build-asan -j$(J)
	UBSAN_OPTIONS=halt_on_error=1 ./build-asan/sediment_tests
	./build-asan/sediment-crashtest --rounds 5 --dir /tmp/sediment-crashtest-asan
	cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEDIMENT_SANITIZE=thread -DSEDIMENT_BUILD_BENCH=OFF
	cmake --build build-tsan -j$(J)
	./build-tsan/sediment_tests

bench: build
	./scripts/bench.sh

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

format:
	$(CLANG_FORMAT) -i $(SOURCES)

clean:
	rm -rf $(BUILD) build-asan build-tsan
