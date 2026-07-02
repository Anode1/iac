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

# Drop the single binary onto $PATH so agents can call plain `iac`.
install: $(BIN)
	install -d $(DESTDIR)$(prefix)/bin
	install -m 755 $(BIN) $(DESTDIR)$(prefix)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(prefix)/bin/$(BIN)

clean:
	rm -f $(BIN) tests

.PHONY: ut install uninstall clean
