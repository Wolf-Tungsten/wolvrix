CPU_MEMORY_STAGE_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_memory_stage_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_memory_stage_test

cpu_memory_stage_test: $(CPU_MEMORY_STAGE_DATA)cpu_memory_stage_main.cpp memory_stage_slots.hpp libgrhsim_cpu_memory_stage.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_memory_stage.a -o $@
