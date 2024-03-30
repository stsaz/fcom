# MD5 hash

include ../config.mk

LIB := MD5.a

default: $(LIB)

# build
CFLAGS := -O3 -fPIC

OBJ := md5.o
%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

clean:
	$(RM) $(OBJ) $(DIR)
