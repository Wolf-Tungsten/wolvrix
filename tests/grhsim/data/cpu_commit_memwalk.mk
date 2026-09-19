CPU_COMMIT_MEMWALK_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_commit_memwalk_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_commit_memwalk_test

cpu_commit_memwalk_test: $(CPU_COMMIT_MEMWALK_DATA)cpu_commit_memwalk_main.cpp libgrhsim_cpu_commit_memwalk.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_commit_memwalk.a -o $@
