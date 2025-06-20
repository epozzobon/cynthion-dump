all: cynthion-dump cynthion-decode

CCFLAGS = -Wall -Werror -O3
ifeq ($(OS),Windows_NT)
CCFLAGS += -lws2_32
endif

cynthion-dump: dump.c
	gcc dump.c -o cynthion-dump -lusb-1.0 $(CCFLAGS)

cynthion-decode: decode.c
	gcc decode.c -o cynthion-decode $(CCFLAGS)

clean:
	rm -f cynthion-dump cynthion-decode