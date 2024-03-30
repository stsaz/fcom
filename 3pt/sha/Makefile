# SHA256/512 hash

include ../config.mk

default: SHA256.a

# build
CFLAGS := -O3 -fPIC

OBJ := sha256.o
%.o: %.c
	$(C) $(CFLAGS) $< -o $@

SHA256.a: sha256.o
	$(AR) rcs $@ $<

clean:
	$(RM) $(OBJ) $(DIR)
