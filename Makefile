CC     ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra
PREFIX ?= /usr/local

whycant: whycant.c
	$(CC) $(CFLAGS) -o $@ whycant.c

install: whycant
	install -D -m 755 whycant $(DESTDIR)$(PREFIX)/bin/whycant

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whycant

clean:
	rm -f whycant

.PHONY: install uninstall clean
