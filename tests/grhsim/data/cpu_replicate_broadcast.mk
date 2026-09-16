CPU_REPLICATE_BROADCAST_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_replicate_broadcast_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_replicate_broadcast_test

cpu_replicate_broadcast_test: $(CPU_REPLICATE_BROADCAST_DATA)cpu_replicate_broadcast_main.cpp libgrhsim_cpu_replicate_broadcast.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_replicate_broadcast.a -o $@
