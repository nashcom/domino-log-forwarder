CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS  = -lcurl

# Targets
TARGET  = otelfwd
TARGET_TEST = otelfwd_unit_test domfwd_durable_test

# The WAL is a module of its own, with its sample, its tests and its README: see wal/README.md
WAL_DIR  = wal
WAL_SRC  = $(WAL_DIR)/simple_wal.cpp
WAL_HDR  = $(WAL_DIR)/simple_wal.hpp

PUSH_HDR = push_status.hpp push_failover.hpp otlp_protobuf.hpp
LOG_HDR  = log_line.hpp

# Targets which are not files. Without this a file named "test" would make "make test" do nothing
.PHONY: all test tsan wal_sample clean

all: $(TARGET)

# Builds and runs the unit tests: the WAL (in wal/, with its sample program), and the pieces of otelfwd (failover between two OTLP
# endpoints, JSON to protobuf converter, log lines). Both run, even if one fails
test: $(TARGET_TEST)
	@fail=0; $(MAKE) -C $(WAL_DIR) test || fail=1; for t in $(TARGET_TEST); do ./$$t || fail=1; done; exit $$fail

# The WAL test with ThreadSanitizer, see wal/makefile. Not part of "make test": it needs g++ with the sanitizer library (libtsan)
tsan:
	$(MAKE) -C $(WAL_DIR) tsan

# The sample program of the WAL, see wal/wal_sample.cpp:  make wal_sample && ./wal/wal_sample
wal_sample:
	$(MAKE) -C $(WAL_DIR) wal_sample

otelfwd: otelfwd.cpp $(WAL_SRC) $(WAL_HDR) $(PUSH_HDR) $(LOG_HDR) health.hpp
	$(CXX) $(CXXFLAGS) -I$(WAL_DIR) -o $@ otelfwd.cpp $(WAL_SRC) $(LDFLAGS)

# Unit test of the failover (push_failover.hpp), of the converter (otlp_protobuf.hpp) and of the log lines (log_line.hpp). No network
# Needs the rapidjson headers
otelfwd_unit_test: otelfwd_unit_test.cpp $(PUSH_HDR) $(LOG_HDR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ otelfwd_unit_test.cpp

# Unit test of the durable line sender of domfwd: the socket sender and the WAL together. It starts a small server of its own on
# a UNIX socket. No Domino, no otelfwd
domfwd_durable_test: domfwd/domfwd_durable_test.cpp domfwd/domfwd_durable.hpp domfwd/domfwd_socket.hpp $(WAL_SRC) $(WAL_HDR)
	$(CXX) $(CXXFLAGS) -pthread -Idomfwd -I$(WAL_DIR) -o $@ domfwd/domfwd_durable_test.cpp $(WAL_SRC)

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
	$(MAKE) -C $(WAL_DIR) clean

