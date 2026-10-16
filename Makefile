CC     ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra
PREFIX ?= /usr/local

whycant: whycant.c
	$(CC) $(CFLAGS) -o $@ whycant.c

install: whycant
	install -D -m 755 whycant $(DESTDIR)$(PREFIX)/bin/whycant

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whycant

# Quick checks that need no root
test: whycant
	./whycant --version
	./whycant --help > /dev/null
	./whycant /etc/hosts > /dev/null
	! ./whycant -u nobody /root/x > /dev/null 2>&1
	@echo "quick tests passed"

# Compare every verdict with the kernel's own decision (needs root)
kernel-test: whycant
	sudo tests/kernel_check.sh ./whycant

clean:
	rm -f whycant

.PHONY: install uninstall test kernel-test clean
