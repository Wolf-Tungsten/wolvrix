CPU_FP_CONE_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_fp_cone_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_fp_cone_test

cpu_fp_cone_test: $(CPU_FP_CONE_DATA)cpu_fp_cone_main.cpp libgrhsim_cpu_fp_cone.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_fp_cone.a -o $@
