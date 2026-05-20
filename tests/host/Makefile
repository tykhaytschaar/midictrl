CC      ?= cc
CFLAGS   = -std=c11 -Wall -Wextra -Werror -O0 -g \
           -I../../components/config/include \
           -I../../components/state_machine/include

BINS     = test_state_machine test_config

all: $(BINS)

test_state_machine: ../../components/state_machine/state_machine.c test_state_machine.c test_helpers.h
	$(CC) $(CFLAGS) -o $@ ../../components/state_machine/state_machine.c test_state_machine.c

test_config: ../../components/config/config_defaults.c test_config.c test_helpers.h
	$(CC) $(CFLAGS) -o $@ ../../components/config/config_defaults.c test_config.c

run: all
	@for bin in $(BINS); do \
	    echo "==== $$bin ===="; \
	    ./$$bin || exit 1; \
	    echo ""; \
	done

clean:
	rm -f $(BINS)
	rm -rf *.dSYM

.PHONY: all run clean
