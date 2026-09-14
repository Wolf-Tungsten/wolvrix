CPU_STATE_SHARE_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check trace reference_trace
check: trace reference_trace
	cmp trace.bin reference/trace.bin
	@echo 'State sharing PASS: complete traces match across 4 init and 32772 evals'

reference_trace:
	$(MAKE) --no-print-directory -C reference -f $(CPU_STATE_SHARE_DATA)cpu_state_share.mk trace

trace: cpu_state_share_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_state_share_test > trace.bin

cpu_state_share_test: $(CPU_STATE_SHARE_DATA)cpu_state_share_main.cpp libgrhsim_cpu_state_share.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_state_share.a -o $@
