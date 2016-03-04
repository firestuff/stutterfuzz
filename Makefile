COMP ?= clang
DISABLED_WARNINGS ?= -Wno-padded
CFLAGS ?= -Weverything -Werror -O3 -g --std=gnu11 --pedantic-errors -fPIE -fstack-protector-strong -D_GNU_SOURCE $(DISABLED_WARNINGS)
LDFLAGS ?= $(CFLAGS) -Wl,-z,relro -Wl,-z,now -pie
LIBS ?=

OBJ = buf.o list.o rand.o

all: stutterfuzz

clean:
	rm -rf *.o stutterfuzz

%.o: %.c *.h
	$(COMP) -c $(CFLAGS) $< -o $@

stutterfuzz: stutterfuzz.o $(OBJ)
	$(COMP) $(LDFLAGS) -o stutterfuzz stutterfuzz.o $(OBJ)
