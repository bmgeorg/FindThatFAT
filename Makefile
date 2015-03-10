CC=clang
CFLAGS=-Wall -g

objects=notjustcats

all: $(objects)

notjustcats: notjustcats.c dataTypes.h helperFunctions.lib Makefile
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(objects)
