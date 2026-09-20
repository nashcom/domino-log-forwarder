PROGRAM=domfwd
TARGET=domfwd
SOURCE= $(PROGRAM).cpp
OBJECT = $(PROGRAM).o

CC=g++
CCOPTS=-c -m64
NOTESDIR=$(Notes_ExecDirectory)

# Direct OTLP push via libcurl (optional, off by default on Linux: events go to otelfwd over the socket).
#   make USE_CURL=1          adds the direct OTLP push (DOMFWD_OtelPushURL) via the libcurl of Domino
#   make CURL_LIBS=-lcurl    if the link fails to resolve curl_* through the Domino libraries
# The curl_* functions are expected to be resolved through the Domino libraries (libnotes), so no
# -lcurl is linked by default. Only the curl headers are needed to compile.
USE_CURL ?= 0
CURL_LIBS ?=

ifneq ($(USE_CURL),0)
CURL_DEFINE = -DDOMFWD_CURL
endif

LIBS=-lnotes -lm -lpthread -lc -ldl $(CURL_LIBS)

INCDIR = $(LOTUS)/notesapi/include
BOOTOBJS = $(LOTUS)/notesapi/lib/linux64/notes0.o $(LOTUS)/notesapi/lib/linux64/notesai0.o
DEFINES = -DGCC3 -DGCC4 -fno-strict-aliasing -DGCC_LBLB_NOT_SUPPORTED -Wformat -Wall -Wcast-align -Wconversion  -DUNIX -DLINUX -DLINUX86 -DND64 -DLINUX64 -DW -DLINUX86_64 -DDTRACE -DPTHREAD_KERNEL -D_REENTRANT -DUSE_THREADSAFE_INTERFACES -D_POSIX_THREAD_SAFE_FUNCTIONS  -DHANDLE_IS_32BITS -DHAS_IOCP -DHAS_BOOL -DHAS_DLOPEN -DUSE_PTHREAD_INTERFACES -DLARGE64_FILES -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -DNDUNIX64 -DLONGIS64BIT -DPRODUCTION_VERSION -DOVERRIDEDEBUG -fPIC -Wno-write-strings $(CURL_DEFINE)

# The WAL (DOMFWD_SocketWAL) is a module of its own, see ../wal/README.md. Linux only. It needs none of the Domino headers or
# defines, so it is compiled by itself. It uses threads: -lpthread is in LIBS
WALDIR    = ../wal
WALOBJECT = simple_wal.o

$(TARGET): $(OBJECT) $(WALOBJECT)
	$(CC) $(OBJECT) $(WALOBJECT) $(BOOTOBJS) -L$(NOTESDIR) -Wl,-rpath-link -no-pie $(NOTESDIR) $(LIBS) -o $(TARGET)

$(OBJECT): $(SOURCE) domfwd_socket.hpp domfwd_durable.hpp ../health.hpp $(WALDIR)/simple_wal.hpp
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) -I$(WALDIR) $(SOURCE) -o $(OBJECT)

$(WALOBJECT): $(WALDIR)/simple_wal.cpp $(WALDIR)/simple_wal.hpp
	$(CC) $(CCOPTS) -std=c++11 -fPIC -Wall -Wextra -I$(WALDIR) $(WALDIR)/simple_wal.cpp -o $(WALOBJECT)

install: $(TARGET) 
	cp -f $(TARGET) /opt/hcl/domino/notes/latest/linux/$(PROGRAM)
	chmod 755  /opt/hcl/domino/notes/latest/linux/$(PROGRAM)

clean:
	rm -f *.o
	rm -f ./$(TARGET)

all:    $(TARGET)
