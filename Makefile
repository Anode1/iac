CC     = cc
CFLAGS = -std=c99 -O2 -W -Wall -Wextra
BIN    = iac
# install location; override e.g. `make install prefix=$HOME/.local`
prefix = /usr/local

$(BIN): iac.c
	$(CC) $(CFLAGS) -o $@ iac.c $(LDFLAGS)

# End-to-end tests drive the built binary, so build it first.
ut: $(BIN) tests
	./tests

tests: tests.c
	$(CC) $(CFLAGS) -o tests tests.c

# Reference examples (not part of iac): the keyboard-priority driver (C) and the
# Telegram <-> board bridge (shell; syntax-checked, needs curl/jq to run).
examples: examples/kbd_driver
	bash -n examples/tg_bridge.sh
	bash -n examples/wa_bridge.sh
examples/kbd_driver: examples/kbd_driver.c
	$(CC) $(CFLAGS) -o $@ examples/kbd_driver.c $(LDFLAGS)

# Sanitizers -- run the end-to-end suite under AddressSanitizer and Undefined-
# BehaviorSanitizer SEPARATELY (a combined run can mask one's reports), the same
# discipline as AIS. Each rebuilds instrumented, runs ./tests, then restores the
# optimized build so a later plain `make ut` isn't silently sanitized. CI runs
# both on Linux AND macOS (.github/workflows/sanitizers.yml -- a different libc/
# allocator catches what Linux-only ASan cannot); the pre-push hook (make hooks)
# runs them before every push.
ASAN  = -fsanitize=address -fno-omit-frame-pointer -g
UBSAN = -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g

ut-asan:
	$(CC) $(CFLAGS) $(ASAN) -o $(BIN) iac.c
	$(CC) $(CFLAGS) $(ASAN) -o tests tests.c
	./tests
	$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory >/dev/null

ut-ubsan:
	$(CC) $(CFLAGS) $(UBSAN) -o $(BIN) iac.c
	$(CC) $(CFLAGS) $(UBSAN) -o tests tests.c
	./tests
	$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory >/dev/null

# Stricter warning gate (matches AIS `make pedantic`): the source must compile
# clean under -pedantic and the prototype/declaration warnings, not just -Wall
# -Wextra. Compile-only -- a warning here is a defect.
PEDFLAGS = -std=c99 -pedantic -Wall -Wextra -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
pedantic:
	$(CC) $(PEDFLAGS) -c -o /dev/null iac.c
	$(CC) $(PEDFLAGS) -c -o /dev/null tests.c

# Enable the git pre-push hook so the sanitizers run before every push -- a
# memory/UB bug can't reach the remote. Bypass once with `git push --no-verify`.
hooks:
	git config core.hooksPath scripts/hooks
	@echo "git hooks -> scripts/hooks  (pre-push runs make ut-asan + ut-ubsan)"

# Drop the single binary onto $PATH so agents can call plain `iac`.
install: $(BIN)
	install -d $(DESTDIR)$(prefix)/bin
	install -m 755 $(BIN) $(DESTDIR)$(prefix)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(prefix)/bin/$(BIN)

clean:
	rm -f $(BIN) tests examples/kbd_driver

.PHONY: ut ut-asan ut-ubsan pedantic hooks examples install uninstall clean
