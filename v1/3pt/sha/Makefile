# SHA512 hash

include ../makeconf

LIB := SHA512.a

default: $(LIB)

# build
CFLAGS := -O3 -fPIC

OBJ := sha512.o
%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

clean:
	$(RM) $(OBJ) $(DIR)
