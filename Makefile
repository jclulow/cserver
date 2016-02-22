
ROOT :=			$(PWD)

CFLAGS =		-I$(ROOT)/include \
			-I$(ROOT)/deps/list/include \
			-std=gnu99 \
			-gdwarf-2 \
			-m32 \
			-Wall -Wextra -Werror \
			-Wno-unused-parameter \
			-Wno-unused-function

LIBS =			-lsocket -lnsl -lumem -lnvpair

TOOLS_PROTO =		/ws/plat/projects/illumos/usr/src/tools/proto/root_i386-nd
CTFCONVERT =		$(TOOLS_PROTO)/opt/onbld/bin/i386/ctfconvert-altexec

POSTPROC_PROG =		$(CTFCONVERT) -l $@ $@

CBUF_OBJS =		cbufq.o \
			cbuf.o \
			cloop.o \
			list.o \
			cserver.o \
			nvpair_json.o \
			json-nvlist.o \
			custr.o

CMON_OBJS =		$(CBUF_OBJS) \
			cmon.o

PROGS =			cmon

.PHONY: all
all: $(PROGS)

obj:
	mkdir -p $@

obj/%.o: src/%.c | obj
	gcc -c $(CFLAGS) -o $@ $^

obj/list.o:
	cd deps/list && gmake BUILD_DIR=$(ROOT)/obj $(ROOT)/obj/list.o

cmon: $(CMON_OBJS:%=obj/%)
	gcc $(CFLAGS) -o $@ $^ $(LIBS)

.PHONY: clean
clean:
	-rm -f obj/*.o
	-rm -f $(PROGS)
