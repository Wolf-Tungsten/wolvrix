CPU_HELPER_READ_CACHE_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_helper_read_cache_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_helper_read_cache_test

cpu_helper_read_cache_test: $(CPU_HELPER_READ_CACHE_DATA)cpu_helper_read_cache_main.cpp libgrhsim_cpu_helper_read_cache.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_helper_read_cache.a -o $@
