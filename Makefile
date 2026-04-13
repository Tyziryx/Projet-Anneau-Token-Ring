CC     = gcc
CFLAGS = -Wall -g
COMMON = utils.c protocole.c

all: driver comm

driver: driver.c $(COMMON)
	$(CC) $(CFLAGS) driver.c $(COMMON) -o driver

comm: comm.c $(COMMON)
	$(CC) $(CFLAGS) comm.c $(COMMON) -o comm

clean:
	rm -f driver comm

.PHONY: all clean