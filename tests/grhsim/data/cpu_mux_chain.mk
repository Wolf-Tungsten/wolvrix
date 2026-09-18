CPU_MUX_CHAIN_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_mux_chain_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_mux_chain_test

cpu_mux_chain_test: $(CPU_MUX_CHAIN_DATA)cpu_mux_chain_main.cpp libgrhsim_cpu_mux_chain.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_mux_chain.a -o $@
