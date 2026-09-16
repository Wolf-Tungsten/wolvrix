CPU_DYNAMIC_STATS_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_dynamic_stats_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_dynamic_stats_test

cpu_dynamic_stats_test: $(CPU_DYNAMIC_STATS_DATA)cpu_dynamic_stats_main.cpp libgrhsim_cpu_dynamic_stats.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_dynamic_stats.a -o $@
