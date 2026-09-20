# The WAL module: the sample program, the unit test, and the test with ThreadSanitizer. See README.md
#
#   make              builds the sample (wal_sample) and the unit test (wal_unit_test)
#   make test         builds and runs the unit test, and builds the sample
#   make tsan         runs the unit test with ThreadSanitizer (needs g++ with the sanitizer library, libtsan)
#   ./wal_sample      runs the sample
#   make clean
#
# This is all a program needs to use the WAL: the two files simple_wal.hpp and simple_wal.cpp, a C++17 compiler, and -pthread
# (the WAL is safe to use from several threads). No libcurl, no rapidjson, nothing else. Copy this folder, or the two files.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

all: wal_sample wal_unit_test

wal_sample: wal_sample.cpp simple_wal.cpp simple_wal.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ wal_sample.cpp simple_wal.cpp

# --wrap lets the test make ftruncate and unlink fail on request (fault injection), without any test code in the WAL
wal_unit_test: wal_unit_test.cpp simple_wal.cpp simple_wal.hpp
	$(CXX) $(CXXFLAGS) -pthread -Wl,--wrap=ftruncate -Wl,--wrap=unlink -o $@ wal_unit_test.cpp simple_wal.cpp

# The sample is built too, so that it cannot go out of date. It is not run
test: wal_unit_test wal_sample
	./wal_unit_test

# Data races between threads, which the checks alone cannot see. The exit code is not 0 if ThreadSanitizer reports a race or a check fails
tsan: wal_unit_test.cpp simple_wal.cpp simple_wal.hpp
	$(CXX) $(CXXFLAGS) -g -fsanitize=thread -pthread -Wl,--wrap=ftruncate -Wl,--wrap=unlink -o wal_unit_test_tsan wal_unit_test.cpp simple_wal.cpp
	./wal_unit_test_tsan -n 20000

clean:
	rm -f wal_sample wal_unit_test wal_unit_test_tsan

.PHONY: all test tsan clean
