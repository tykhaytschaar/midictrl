CC      ?= cc
CFLAGS   = -std=c11 -Wall -Wextra -Werror -O0 -g \
           -I../../components/config/include \
           -I../../components/state_machine/include
SRCS     = ../../components/state_machine/state_machine.c \
           test_state_machine.c
OUT      = test_state_machine

$(OUT): $(SRCS) test_helpers.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRCS)

run: $(OUT)
	./$(OUT)

clean:
	rm -f $(OUT)

.PHONY: run clean
