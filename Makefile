.POSIX:
CC = cc
CFLAGS = -O2 -Os -Wall -Wextra

clil: clil.c
	$(CC) $(CFLAGS) -o clil clil.c
	strip clil

clean:
	rm -f clil
