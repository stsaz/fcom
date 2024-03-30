# libpng

include ../config.mk

VER := 1.6.37
URL := https://download.sourceforge.net/libpng/libpng-$(VER).tar.xz
PKG := $(notdir $(URL))
DIR := libpng-$(VER)
LIB := libpng-ff.$(SO)

CONFIGURE_FLAGS :=
ifeq ($(OS),windows)
	CONFIGURE_FLAGS += --host=x86_64-w64-mingw32
endif
all: $(DIR)
	cd $(DIR) && cat ../*.patch | patch -p1
	cp $(DIR)/scripts/pnglibconf.h.prebuilt $(DIR)/pnglibconf.h
	cd $(DIR) && ./configure $(CONFIGURE_FLAGS)
	$(MAKE) -rR $(LIB)

# download
$(PKG):
	$(CURL) -o $(PKG) $(URL)

# unpack
$(DIR): $(PKG)
	$(UNTAR_XZ) $(PKG)
	touch $(DIR)

# build
CFLAGS += \
	-I$(DIR) -I../zlib/zlib-1.2.11 \
	-DHAVE_CONFIG_H -DHAVE_LIBZ=1

SRC := $(DIR)/png.o \
	$(DIR)/pngerror.o \
	$(DIR)/pngget.o \
	$(DIR)/pngmem.o \
	$(DIR)/pngpread.o \
	$(DIR)/pngread.o \
	$(DIR)/pngrio.o \
	$(DIR)/pngrtran.o \
	$(DIR)/pngrutil.o \
	$(DIR)/pngset.o \
	$(DIR)/pngtrans.o \
	$(DIR)/pngwio.o \
	$(DIR)/pngwrite.o \
	$(DIR)/pngwtran.o \
	$(DIR)/pngwutil.o

OBJ := png-ff.o $(SRC:.c=.o)

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(LINK) -shared $(OBJ) ../zlib/libz.a $(LINKFLAGS) -o $@

clean:
	$(RM) $(OBJ) $(DIR)
