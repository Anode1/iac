CC     = cc
CFLAGS = -std=c99 -O2 -W -Wall -Wextra
BIN    = iac

$(BIN): iac.c
	$(CC) $(CFLAGS) -o $@ iac.c

# End-to-end tests drive the built binary, so build it first.
ut: $(BIN) tests
	./tests

tests: tests.c
	$(CC) $(CFLAGS) -o tests tests.c

clean:
	rm -f $(BIN) tests

.PHONY: ut clean
