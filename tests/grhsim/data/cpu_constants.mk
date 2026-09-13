CPU_CONSTANT_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_constants_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_constants_test

cpu_constants_test: $(CPU_CONSTANT_DATA)cpu_constants_main.cpp libgrhsim_cpu_constants.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_constants.a -o $@
