# autodj — console AutoDJ. Headers under third_party are the system
# libmpg123, libsoxr, and ALSA development headers used at compile time.
# The binary links the shared libraries already installed on the machine.
# Loudness measurement is libebur128 (MIT), compiled from third_party/ebur128.

CC      := gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -MMD -MP -Isrc -Ithird_party/include -Ithird_party/ebur128
LDFLAGS := -pthread
LDLIBS  := /usr/lib/aarch64-linux-gnu/libmpg123.so.0 \
           /usr/lib/aarch64-linux-gnu/libsoxr.so.0 \
           /usr/lib/aarch64-linux-gnu/libasound.so.2 \
           -lm

PREFIX  := /home/pi/bin
SRC     := src/main.c src/log.c src/config.c src/playlist.c src/decode.c \
           src/player.c src/audio.c src/keys.c src/cfgedit.c src/fx.c
OBJ     := $(SRC:.c=.o)
EBUR    := third_party/ebur128/ebur128.o
DEP     := $(OBJ:.o=.d) third_party/ebur128/ebur128.d

.PHONY: all install clean

all: autodj

autodj: $(OBJ) $(EBUR)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(EBUR) $(LDLIBS)

src/keys.o: src/keytab.inc

src/keytab.inc: /usr/include/linux/input-event-codes.h
	awk '/^#define[ \t]+KEY_[A-Z0-9_]+[ \t]+(0x[0-9A-Fa-f]+|[0-9]+)([ \t]|$$)/ { \
		name = $$2; \
		if (name == "KEY_MAX" || name == "KEY_CNT" || name == "KEY_MIN_INTERESTING") next; \
		print "X(" name ")"; \
	}' $< > $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

third_party/ebur128/ebur128.o: third_party/ebur128/ebur128.c third_party/ebur128/ebur128.h
	$(CC) $(CFLAGS) -Wno-cpp -Wno-unused-but-set-variable -c -o $@ $<

-include $(DEP)

install: autodj
	install -d $(PREFIX)
	install -m 755 autodj $(PREFIX)/autodj

clean:
	rm -f $(OBJ) $(EBUR) $(DEP) autodj src/keytab.inc
