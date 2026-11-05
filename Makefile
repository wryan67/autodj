# autodj — console AutoDJ. Headers under third_party are the system
# libmpg123, libsoxr, and ALSA development headers used at compile time.
# The binary links the shared libraries already installed on the machine.

CC      := gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -MMD -MP -Isrc -Ithird_party/include
LDFLAGS := -pthread
LDLIBS  := /usr/lib/aarch64-linux-gnu/libmpg123.so.0 \
           /usr/lib/aarch64-linux-gnu/libsoxr.so.0 \
           /usr/lib/aarch64-linux-gnu/libasound.so.2 \
           -lm

PREFIX  := /home/pi/bin
SRC     := src/main.c src/log.c src/config.c src/playlist.c src/decode.c \
           src/player.c src/audio.c src/keys.c
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

.PHONY: all install clean

all: autodj

autodj: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/keys.o: src/keytab.inc

src/keytab.inc: /usr/include/linux/input-event-codes.h
	awk '/^#define[ \t]+KEY_[A-Z0-9_]+[ \t]+(0x[0-9A-Fa-f]+|[0-9]+)([ \t]|$$)/ { \
		name = $$2; \
		if (name == "KEY_MAX" || name == "KEY_CNT" || name == "KEY_MIN_INTERESTING") next; \
		print "X(" name ")"; \
	}' $< > $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

install: autodj
	install -d $(PREFIX)
	install -m 755 autodj $(PREFIX)/autodj

clean:
	rm -f $(OBJ) $(DEP) autodj src/keytab.inc
