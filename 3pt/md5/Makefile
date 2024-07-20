# MD5 hash

include ../config.mk

default: MD5.a

%.o: $(FCOM_3PT)/md5/%.c
	$(C) $(CFLAGS) $< -o $@

MD5.a: md5.o
	$(AR) rcs $@ $+

clean:
	$(RM) *.o $(DIR)
