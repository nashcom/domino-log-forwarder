# The MailLog module, its unit test and its sample program. See README.md
#
#   make              builds the unit test (mail_log_unit_test) and the sample (mail_log_sample)
#   make test         builds and runs the unit test (the sample is built too, so that it cannot go out of date. It is not run)
#   ./mail_log_sample  runs the sample
#   make clean
#
# This is all a program needs to use it: the two files mail_log.hpp and mail_log.cpp, and a C++17 compiler. No libcurl, no
# rapidjson, no threads, nothing else. Copy this folder, or the two files.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

all: mail_log_unit_test mail_log_sample

mail_log_unit_test: mail_log_unit_test.cpp mail_log.cpp mail_log.hpp
	$(CXX) $(CXXFLAGS) -o $@ mail_log_unit_test.cpp mail_log.cpp

mail_log_sample: mail_log_sample.cpp mail_log.cpp mail_log.hpp
	$(CXX) $(CXXFLAGS) -o $@ mail_log_sample.cpp mail_log.cpp

test: mail_log_unit_test mail_log_sample
	./mail_log_unit_test

clean:
	rm -f mail_log_unit_test mail_log_sample

.PHONY: all test clean
