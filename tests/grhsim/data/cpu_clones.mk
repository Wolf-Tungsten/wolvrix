CPU_CLONES_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_clones_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_clones_test

cpu_clones_test: $(CPU_CLONES_DATA)cpu_clones_main.cpp libgrhsim_cpu_clones.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_clones.a -o $@
