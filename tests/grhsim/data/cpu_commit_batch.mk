CPU_COMMIT_BATCH_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_commit_batch_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_commit_batch_test

cpu_commit_batch_test: $(CPU_COMMIT_BATCH_DATA)cpu_commit_batch_main.cpp libgrhsim_cpu_commit_batch.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_commit_batch.a -o $@
