cynthion-dump: dump.c
	gcc dump.c -o cynthion-dump -lusb-1.0 -Wall -Werror -O3

clean:
	rm cynthion-dump