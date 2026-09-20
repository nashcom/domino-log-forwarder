# The FileReader module and its unit test. See README.md
#
#   make              builds the unit test (file_reader_unit_test)
#   make test         builds and runs the unit test
#   make clean
#
# This is all a program needs to use the reader: the two files file_reader.hpp and file_reader.cpp, and a C++17 compiler.
# No libcurl, no rapidjson, no threads, nothing else. Copy this folder, or the two files.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

all: file_reader_unit_test

file_reader_unit_test: file_reader_unit_test.cpp file_reader.cpp file_reader.hpp
	$(CXX) $(CXXFLAGS) -o $@ file_reader_unit_test.cpp file_reader.cpp

test: file_reader_unit_test
	./file_reader_unit_test

clean:
	rm -f file_reader_unit_test

.PHONY: all test clean
