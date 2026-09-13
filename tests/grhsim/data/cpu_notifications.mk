CPU_NOTIFICATIONS_DATA := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include Makefile

.PHONY: check
check: cpu_notifications_test
	ASAN_OPTIONS=detect_leaks=0 prlimit --stack=8388608:8388608 -- ./cpu_notifications_test

cpu_notifications_test: $(CPU_NOTIFICATIONS_DATA)cpu_notifications_main.cpp libgrhsim_cpu_notifications.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_cpu_notifications.a -o $@
