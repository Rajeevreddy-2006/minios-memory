# minios-memory: a memory manager for a toy OS (C kernel core, C++ tests and benchmarks)
CC       ?= gcc
CXX      ?= g++
WARN     := -Wall -Wextra -Wpedantic
CFLAGS   := -std=c11   $(WARN) -O2 -g -Ikernel
CXXFLAGS := -std=c++17 $(WARN) -O2 -g -Ikernel -Itests
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD := build
KSRC  := mm pagesim buddy slab lz4
KOBJ  := $(KSRC:%=$(BUILD)/%.o)          # sanitizer build, used by the tests
ROBJ  := $(KSRC:%=$(BUILD)/rel/%.o)      # plain -O2 build, used by the benchmarks (real timings)
TESTS := test_mm test_buddy test_slab test_lz4 test_zpool

.PHONY: all test bench clean
all: $(TESTS:%=$(BUILD)/%) $(BUILD)/bench_paging $(BUILD)/bench_mem

$(BUILD) $(BUILD)/rel:
	mkdir -p $@

$(BUILD)/%.o: kernel/%.c kernel/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -c $< -o $@
$(BUILD)/rel/%.o: kernel/%.c kernel/*.h | $(BUILD)/rel
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_%: tests/test_%.cpp $(KOBJ) tests/minitest.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SAN) $< $(KOBJ) -o $@
$(BUILD)/bench_%: sim/bench_%.cpp $(ROBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(ROBJ) -o $@

test: $(TESTS:%=$(BUILD)/%)
	@for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t || exit 1; done

bench: $(BUILD)/bench_mem $(BUILD)/bench_paging
	./$(BUILD)/bench_mem
	./$(BUILD)/bench_paging

clean:
	rm -rf $(BUILD)
