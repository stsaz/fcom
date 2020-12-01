# fcom v0.1 makefile

PROJ := fcom
ROOT := ..
PROJDIR := $(ROOT)/fcom
SRCDIR := $(PROJDIR)/src
OPT := LTO3

FFBASE := $(ROOT)/ffbase
FFOS := $(ROOT)/ffos
FFPACK := $(ROOT)/ffpack
FF := $(ROOT)/ff
FF3PT := $(ROOT)/ff-3pt

include $(FFOS)/makeconf

ifeq ($(OS),win)
INSTDIR := ./$(PROJ)
BIN := fcom.exe
CFLAGS += -DFF_WIN_APIVER=0x0501

else
INSTDIR := ./$(PROJ)-0
BIN := fcom
endif
VER :=

FF_OBJ_DIR := ./ff-obj
ifeq ($(OPT),0)
	CFLAGS_OPT += -DFF_DEBUG
endif
# CFLAGS += -fsanitize=address
# LDFLAGS += -fsanitize=address -ldl
CFLAGS += -DFFBASE_HAVE_FFERR_STR
FFOS_CFLAGS := $(CFLAGS) $(CFLAGS_OPT)
FF_CFLAGS := $(CFLAGS) $(CFLAGS_OPT)
FF3PT_CFLAGS := $(CFLAGS) $(CFLAGS_OPT)
FF3PTLIB := $(FF3PT)-bin/$(OS)-$(ARCH)

CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers \
	-I$(SRCDIR) -I$(FFBASE) -I$(FFPACK) -I$(FF) -I$(FFOS) -I$(FF3PT)

LDFLAGS += -L$(FFPACK)/zlib -L$(FFPACK)/lzma -L$(FF3PTLIB)

include $(PROJDIR)/makerules


package:
	rm -f $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) \
		&&  $(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH)-debug.$(PACK_EXT) ./*.debug
