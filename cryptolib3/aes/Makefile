# Brian Gladman's AES

include ../makeconf

VER :=
URL := https://github.com/BrianGladman/aes/archive/refs/heads/master.zip
PKG := $(notdir $(URL))
DIR := aes-master
LIB := AES-ff.a

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
CFLAGS := -O3 -I$(DIR) -fPIC

SRC := \
	$(DIR)/aescrypt.c \
	$(DIR)/aeskey2.c \
	$(DIR)/aeskey.c \
	$(DIR)/aes_modes.c \
	$(DIR)/aes_ni.c \
	$(DIR)/aestab.c
OBJ := aes-ff.o $(SRC:.c=.o)

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

clean:
	$(RM) $(OBJ) $(DIR)
