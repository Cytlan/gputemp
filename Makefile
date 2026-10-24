CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=c99
LDLIBS  += -ldl
PREFIX  ?= /usr/local

gputemp: gputemp.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: gputemp
	install -Dm755 gputemp $(DESTDIR)$(PREFIX)/bin/gputemp

clean:
	rm -f gputemp

.PHONY: install clean
