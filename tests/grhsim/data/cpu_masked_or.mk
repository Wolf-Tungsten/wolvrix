include Makefile

.PHONY: check
check: masked_or_test
	./masked_or_test

masked_or_test: driver.cpp libgrhsim_masked_or.a
	$(CXX) $(CXXFLAGS) -I. $< libgrhsim_masked_or.a -o $@
