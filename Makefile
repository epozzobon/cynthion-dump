all: cynthion-dump cynthion-decode


cynthion-dump: dump.c
	gcc dump.c -o cynthion-dump -lusb-1.0 -Wall -Werror -O3

cynthion-decode: decode.c
	gcc decode.c -o cynthion-decode -lusb-1.0 -Wall -Werror -O3

clean:
	rm -f cynthion-dump cynthion-decode