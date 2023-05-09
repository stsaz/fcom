# SHA256/512 hash

include ../makeconf

LIB1 := SHA1.a
LIB256 := SHA256.a
LIB512 := SHA512.a

default: $(LIB1) $(LIB256) $(LIB512)

# build
CFLAGS := -O3 -fPIC

OBJ := sha1.o sha256.o sha512.o
%.o: %.c
	$(C) $(CFLAGS) $< -o $@

SHA1.a: sha1.o
	$(AR) rcs $@ $<
SHA256.a: sha256.o
	$(AR) rcs $@ $<
SHA512.a: sha512.o
	$(AR) rcs $@ $<

clean:
	$(RM) $(OBJ) $(DIR)
