CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS  = -lcurl

# Targets
TARGET  = otelfwd
TARGET_TEST = wal_unit_test

# Common sources
WAL_SRC  = simple_wal.cpp
WAL_HDR  = simple_wal.hpp

# Targets which are not files. Without this a file named "test" would make "make test" do nothing
.PHONY: all test clean

all: $(TARGET)

# Builds and runs the unit test of the WAL
test: $(TARGET_TEST)
	./$(TARGET_TEST)

otelfwd: otelfwd.cpp $(WAL_SRC) $(WAL_HDR)
	$(CXX) $(CXXFLAGS) -o $@ otelfwd.cpp $(WAL_SRC) $(LDFLAGS)

# Unit test of the WAL module alone: no otelfwd, no network. Also measures the throughput of the WAL
wal_unit_test: wal_unit_test.cpp $(WAL_SRC) $(WAL_HDR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ wal_unit_test.cpp $(WAL_SRC)

# Test tool: receiving end of the OTLP test container, see tools/otel-sink/. Not part of "all".
#   make otel-sink                needs rapidjson and zlib headers
otel-sink: tools/otel-sink/otel-sink.cpp tools/otel-sink/test_ledger.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ tools/otel-sink/otel-sink.cpp -lz

# Test tool: load test of NGINX -> otelfwd -> otel-sink, see nginx/. Not part of "all".
#   make loadtest                 needs the rapidjson headers
loadtest: nginx/loadtest.cpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ nginx/loadtest.cpp

clean:
	rm -f $(TARGET)
	rm -f $(TARGET_TEST)
	rm -f otel-sink
	rm -f loadtest
	rm -f *.o

