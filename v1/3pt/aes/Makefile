# Brian Gladman's AES

include ../makeconf

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
CFLAGS := -O3 -D_LIB -DASM_AMD64_C -I$(DIR) -fPIC

YASM := yasm
YASM_FLAGS := -f elf64
ifeq "$(OS)" "windows"
	YASM_FLAGS := -f win64
endif
YASM_FLAGS += -m amd64 -DASM_AMD64_C -DINTEL_AES_POSSIBLE

SRC := \
	$(DIR)/aes_amd64.asm \
	$(DIR)/aeskey.c \
	$(DIR)/aes_modes.c \
	$(DIR)/aes_ni.c \
	$(DIR)/aestab.c
OBJ := aes-ff.o $(SRC:.c=.o) $(SRC:.asm=.o)

%.o: %.c
	$(C) $(CFLAGS) $< -o $@
%.o: %.asm
	$(YASM) $(YASM_FLAGS) $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

clean:
	$(RM) $(OBJ) $(DIR)
