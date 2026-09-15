CPU_PREDICATES_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_predicates_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_predicates_test

cpu_predicates_test: $(CPU_PREDICATES_DATA)cpu_predicates_main.cpp libgrhsim_cpu_predicates.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_predicates.a -o $@
