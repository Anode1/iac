CC     = cc
CFLAGS = -std=c99 -O2 -W -Wall -Wextra
BIN    = iac
# install location; override e.g. `make install prefix=$HOME/.local`
prefix = /usr/local

$(BIN): iac.c
	$(CC) $(CFLAGS) -o $@ iac.c

# End-to-end tests drive the built binary, so build it first.
ut: $(BIN) tests
	./tests

tests: tests.c
	$(CC) $(CFLAGS) -o tests tests.c

# Same suite under AddressSanitizer + UBSan (catches races/overflows the
# fork-storm cases can trip). Rebuilds instrumented, runs, restores the
# optimized build so a later plain `make ut` isn't silently sanitized.
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -g
ut-asan:
	$(CC) $(CFLAGS) $(SAN) -o $(BIN) iac.c
	$(CC) $(CFLAGS) $(SAN) -o tests tests.c
	./tests
	$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory >/dev/null

# Drop the single binary onto $PATH so agents can call plain `iac`.
install: $(BIN)
	install -d $(DESTDIR)$(prefix)/bin
	install -m 755 $(BIN) $(DESTDIR)$(prefix)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(prefix)/bin/$(BIN)

clean:
	rm -f $(BIN) tests

.PHONY: ut ut-asan install uninstall clean
