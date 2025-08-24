CC = gcc
CFLAGS = -c -I. -Wall -Wextra

all: emisor receptor

emisor: emisor.o protocolo.o
	$(CC) -o emisor emisor.o protocolo.o 

receptor: receptor.o protocolo.o
	$(CC) -o receptor receptor.o protocolo.o

emisor.o: emisor.c protocolo.h
	$(CC) $(CFLAGS) emisor.c

receptor.o: receptor.c protocolo.h
	$(CC) $(CFLAGS) receptor.c

protocolo.o: protocolo.c protocolo.h
	$(CC) $(CFLAGS) protocolo.c

.PHONY: clean
clean:
	rm -f emisor receptor *.o *.txt
