# Brian Gladman's AES

include ../config.mk

VER :=
URL := https://github.com/BrianGladman/aes/archive/refs/heads/master.zip
PKG := $(FCOM_3PT)/aes/$(notdir $(URL))
DIR := aes-master
LIB := AES.a

default: $(DIR)
	cat $(FCOM_3PT)/aes/*.patch | patch -p1 -d $(DIR)/
	$(SUBMAKE) $(LIB)

# download
$(PKG):
	$(CURL) -o $@ $(URL)

# unpack
$(DIR): $(PKG)
	$(UNZIP) $(PKG)

# build
CFLAGS += \
	-DASM_AMD64_C -DINTEL_AES_POSSIBLE \
	-I$(DIR)

YASM := yasm
YASM_FLAGS := -f elf64
ifeq "$(OS)" "windows"
	YASM_FLAGS := -f win64
endif
YASM_FLAGS += -m amd64 -DASM_AMD64_C -DINTEL_AES_POSSIBLE

%.o: $(FCOM_3PT)/aes/%.c
	$(C) $(CFLAGS) $< -o $@

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

%.o: %.asm
	$(YASM) $(YASM_FLAGS) $< -o $@

OBJ := aes-ff.o \
	$(DIR)/aes_amd64.o \
	$(DIR)/aeskey.o \
	$(DIR)/aes_modes.o \
	$(DIR)/aes_ni.o \
	$(DIR)/aestab.o

$(LIB): $(OBJ)
	$(RM) $@
	$(AR) rcs $@ $+

clean:
	$(RM) *.o $(DIR)
