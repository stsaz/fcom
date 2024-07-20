# SHA256 hash

include ../config.mk

default: SHA256.a

%.o: $(FCOM_3PT)/sha/%.c
	$(C) $(CFLAGS) $< -o $@

SHA256.a: sha256.o
	$(AR) rcs $@ $+

clean:
	$(RM) *.o $(DIR)
