CFLAGS ?= -std=c11 -Wall -Wextra -O2

test_snd: test_snd.c snd.c snd.h snd_notes.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_snd.c snd.c -o $@ -lpthread

test_snd_tsan: test_snd.c snd.c snd.h snd_notes.h
	$(CC) $(CFLAGS) -g -fsanitize=thread test_snd.c snd.c -o $@ -lpthread

check: test_snd test_snd_tsan
	./test_snd
	setarch "$$(uname -m)" -R ./test_snd_tsan   # TSAN needs address randomisation off on newer kernels

clean:
	rm -f test_snd test_snd_tsan
