CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

CPPFLAGS += -Ishim -I../kernel

all: test_airmouse_math

test_airmouse_math: test_airmouse_math.c ../kernel/lg_magic_airmouse.c ../kernel/lg_magic_airmouse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_airmouse_math.c ../kernel/lg_magic_airmouse.c -lm

check: test_airmouse_math
	./test_airmouse_math

clean:
	rm -f test_airmouse_math

.PHONY: all check clean
