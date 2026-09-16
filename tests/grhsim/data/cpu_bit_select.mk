CPU_BIT_SELECT_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_bit_select_test
	ASAN_OPTIONS=detect_leaks=0 ./cpu_bit_select_test

cpu_bit_select_test: $(CPU_BIT_SELECT_DATA)cpu_bit_select_main.cpp libgrhsim_cpu_bit_select.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_bit_select.a -o $@
