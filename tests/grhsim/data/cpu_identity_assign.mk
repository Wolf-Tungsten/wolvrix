CPU_IDENTITY_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_identity_assign_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_identity_assign_test

cpu_identity_assign_test: $(CPU_IDENTITY_DATA)cpu_identity_assign_main.cpp libgrhsim_cpu_identity_assign.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_identity_assign.a -o $@
