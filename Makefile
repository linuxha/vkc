# Linux side: kctool and the codec tests.  The firmware builds with CMake
# (see "make firmware" or firmware/README in the top-level README).

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror=implicit-function-declaration -Icommon
LDLIBS  += -lm

B := build

all: $(B)/kctool

$(B):
	mkdir -p $@

$(B)/kctool: tools/kctool.c tools/kcnet.c common/kcs.c common/kcs.h common/vkc_proto.h tools/kctool.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/kctool.c tools/kcnet.c common/kcs.c $(LDLIBS)

$(B)/test_kcs: tests/test_kcs.c common/kcs.c common/kcs.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_kcs.c common/kcs.c $(LDLIBS)

test: $(B)/test_kcs $(B)/kctool
	$(B)/test_kcs
	KCTOOL=$(B)/kctool sh tests/test_kctool.sh

firmware:
	cmake -S firmware -B $(B)/firmware -DPICO_BOARD=pico_w
	$(MAKE) -C $(B)/firmware -j

install: $(B)/kctool
	install -D -m 755 $(B)/kctool $(DESTDIR)$(PREFIX)/bin/kctool

PREFIX ?= /usr/local

clean:
	rm -rf $(B)

.PHONY: all test firmware install clean
