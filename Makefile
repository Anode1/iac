# iac -- inter-agent communication. ISC License; see LICENSE.
#
# The engine is one binary built from every *.c except the test driver; drop in
# a new module (say ping.c/ping.h) and it is picked up with no edit here. The
# feature-test macros live in CPPFLAGS so each .c stays clean: flock/LOCK_* are
# BSD (need _DEFAULT_SOURCE on glibc), inotify needs _DEFAULT_SOURCE, and macOS
# hides both under strict _POSIX_C_SOURCE unless _DARWIN_C_SOURCE is set.
CC     = cc
CFLAGS = -std=c99 -O2 -W -Wall -Wextra
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
BIN    = iac
# install location; override e.g. `make install prefix=$HOME/.local`
prefix = /usr/local

# engine = all top-level *.c except the end-to-end test driver (tests.c)
SRC  = $(filter-out tests.c, $(wildcard *.c))
OBJS = $(SRC:.c=.o)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -c $< -o $@

# End-to-end tests drive the built binary, so build it first. tests.c carries its
# own feature-test #defines, so it compiles without CPPFLAGS.
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
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ASAN) -o $(BIN) $(SRC)
	$(CC) $(CFLAGS) $(ASAN) -o tests tests.c
	./tests
	$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory >/dev/null

ut-ubsan:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(UBSAN) -o $(BIN) $(SRC)
	$(CC) $(CFLAGS) $(UBSAN) -o tests tests.c
	./tests
	$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory >/dev/null

# Stricter warning gate (matches AIS `make pedantic`): every source must compile
# clean under -pedantic and the prototype/declaration warnings, not just -Wall
# -Wextra. Compile-only -- a warning here is a defect.
PEDFLAGS = -std=c99 -pedantic -Wall -Wextra -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
pedantic:
	for f in $(SRC); do $(CC) $(PEDFLAGS) $(CPPFLAGS) -c $$f -o /dev/null || exit 1; done
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
	rm -f $(BIN) tests examples/kbd_driver $(OBJS) $(OBJS:.o=.d)

-include $(OBJS:.o=.d)

.PHONY: ut ut-asan ut-ubsan pedantic hooks examples install uninstall clean
