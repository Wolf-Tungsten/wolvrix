CPU_PACKED_BITS_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check trace reference_trace
check: trace reference_trace
	cmp trace.bin reference/trace.bin
	@echo 'Packed bit registers PASS: scoreboard and complete DPI traces match'

reference_trace:
	$(MAKE) --no-print-directory -C reference -f $(CPU_PACKED_BITS_DATA)cpu_packed_bits.mk trace

trace: cpu_packed_bits_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_packed_bits_test > trace.bin

cpu_packed_bits_test: $(CPU_PACKED_BITS_DATA)cpu_packed_bits_main.cpp libgrhsim_cpu_packed_bits.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_packed_bits.a -o $@
