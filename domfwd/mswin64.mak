# Windows 64-bit version using 
# Microsoft Visual Studio 2017

PROGRAM=domfwd

NODEBUG=1

# Direct OTLP push via libcurl (optional, on by default).
#   nmake /f mswin64.mak USE_CURL=0    builds without any libcurl dependency (socket transport to the forwarder only)
# The libcurl bundled in nnotes.dll is used, see libcurl-x64.lib below. The curl SDK headers are only needed for the types.

!IF "$(USE_CURL)" == "0"
CURL_DEFINE =
CURL_LIB    =
CURL_INC    =
!ELSE
CURL_DEFINE =-DDOMFWD_CURL
CURL_LIB    =libcurl-x64.lib
CURL_INC    =-I"N:\curl\include"
!ENDIF

# Link command

n$(PROGRAM).exe: $(PROGRAM).obj domfwd_ext.lib $(CURL_LIB)
	link /SUBSYSTEM:CONSOLE $(PROGRAM).obj notes0.obj notesai0.obj notes.lib domfwd_ext.lib $(CURL_LIB) ws2_32.lib msvcrt.lib user32.lib /PDB:$*.pdb /DEBUG /PDBSTRIPPED:$*_small.pdb -out:$@
	del $*.pdb $*.sym
	rename $*_small.pdb $*.pdb

# Compile command

$(PROGRAM).obj: $(PROGRAM).cpp domfwd_socket.hpp domfwd_durable.hpp
	cl -nologo -c -D_MT -MT /Zi /Ot /O2 /Ob2 /Oy- -Gd /Gy /GF /Gs4096 /GS- /favor:INTEL64 /EHsc /Zc:wchar_t- /DWINVER=0x0602 -Zl -W1 -DNT -DW32 -DW -DW64 -DND64 -D_AMD64_ -DDTRACE -D_CRT_SECURE_NO_WARNINGS -DND64SERVER -DPRODUCTION_VERSION /DUSE_WIN32_IDN $(CURL_DEFINE) $(CURL_INC) $*.cpp

# Import lib for undocumented event extraction exports, not present in notes.lib

domfwd_ext.lib: domfwd_ext.def
	lib /DEF:domfwd_ext.def /MACHINE:X64 /OUT:domfwd_ext.lib

# Import lib for the curl_* functions bundled inside nnotes.dll itself -
# undocumented, same idiom as domfwd_ext.lib. Only used when libcurl support
# is built in (default, see USE_CURL above). No separate DLL to place
# at runtime - it's already part of every Domino install.

libcurl-x64.lib: libcurl-x64.def
	lib /DEF:libcurl-x64.def /MACHINE:X64 /OUT:libcurl-x64.lib

all:
	n$(PROGRAM).exe

clean:
	del *.obj *.pdb *.exe *.dll *.ilk *.sym *.map *.lib *.exp

