# Brian Gladman's AES

include ../config.mk

VER :=
URL := https://github.com/BrianGladman/aes/archive/refs/heads/master.zip
PKG := $(notdir $(URL))
DIR := aes-master
LIB := AES.a

all: $(DIR)
	$(MAKE) -rR $(LIB)

# download
$(PKG):
	$(CURL) -o $(PKG) $(URL)

# unpack
$(DIR): $(PKG)
	$(UNZIP) $(PKG)
	touch $(DIR)

# build
CFLAGS := -O3 -DASM_AMD64_C -DINTEL_AES_POSSIBLE -I$(DIR) -fPIC

YASM := yasm
YASM_FLAGS := -f elf64
ifeq "$(OS)" "windows"
	YASM_FLAGS := -f win64
endif
YASM_FLAGS += -m amd64 -DASM_AMD64_C -DINTEL_AES_POSSIBLE

OBJ := aes-ff.o \
	$(DIR)/aes_amd64.o \
	$(DIR)/aeskey.o \
	$(DIR)/aes_modes.o \
	$(DIR)/aes_ni.o \
	$(DIR)/aestab.o

%.o: %.c
	$(C) $(CFLAGS) $< -o $@
%.o: %.asm
	$(YASM) $(YASM_FLAGS) $< -o $@

$(LIB): $(OBJ)
	rm $@ || true
	$(AR) rcs $@ $(OBJ)

clean:
	$(RM) $(OBJ) $(DIR)
