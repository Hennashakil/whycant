CC     ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra
PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man

# Hardened build used for releases and the .deb package
HARDEN = -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fstack-clash-protection \
         -fPIE -pie -Wl,-z,relro,-z,now

whycant: whycant.c
	$(CC) $(CFLAGS) -o $@ whycant.c

release: whycant.c
	$(CC) -std=c17 -O2 -Wall -Wextra $(HARDEN) -o whycant whycant.c
	strip whycant

install: whycant
	install -D -m 755 whycant $(DESTDIR)$(PREFIX)/bin/whycant
	install -D -m 644 whycant.1 $(DESTDIR)$(MANDIR)/man1/whycant.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whycant $(DESTDIR)$(MANDIR)/man1/whycant.1

deb:
	packaging/build-deb.sh

# Quick checks that need no root
test: whycant
	./whycant --version
	./whycant --help > /dev/null
	./whycant Makefile > /dev/null
	! ./whycant -u nobody /root/x > /dev/null 2>&1
	./whycant --json Makefile | grep -q '"result":"allowed"'
	@echo "quick tests passed"

# Compare every verdict with the kernel's own decision (needs root)
kernel-test: whycant
	sudo tests/kernel_check.sh ./whycant

clean:
	rm -f whycant *.deb

.PHONY: release install uninstall deb test kernel-test clean
