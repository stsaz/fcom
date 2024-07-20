# libz for libpng

include ../config.mk

VER := 1.2.11
URL := https://www.zlib.net/zlib-$(VER).tar.xz
PKG := $(FCOM_3PT)/zlib/$(notdir $(URL))
DIR := zlib-$(VER)
LIB := libz.a

default: $(DIR)
	$(SUBMAKE) $(LIB)

# download
$(PKG):
	$(CURL) -o $(PKG) $(URL)

# unpack
$(DIR): $(PKG)
	$(UNTAR_XZ) $(PKG)

# build
CFLAGS += -I$(DIR) \
	-DZ_EXP -DNO_GZIP -DHAVE_UNISTD_H -DHAVE_STDARG_H -D_LARGEFILE64_SOURCE=1
ifneq "$(OS)" "windows"
	CFLAGS += -DHAVE_HIDDEN
endif

SRC := $(DIR)/adler32.c \
	$(DIR)/crc32.c \
	$(DIR)/deflate.c \
	$(DIR)/infback.c \
	$(DIR)/inffast.c \
	$(DIR)/inflate.c \
	$(DIR)/inftrees.c \
	$(DIR)/trees.c \
	$(DIR)/zutil.c
OBJ := $(SRC:.c=.o)

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $+

clean:
	$(RM) $(OBJ) $(DIR)
