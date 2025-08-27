CFLAGS=-ggdb -std=c11 -Wall -Wextra

bkf: blackknifeforth.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm bkf
