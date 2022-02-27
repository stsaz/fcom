# fcom

PROJ := fcom
ROOT := ..
PROJDIR := $(ROOT)/fcom
SRCDIR := $(PROJDIR)/src
OPT := LTO3

FFBASE := $(ROOT)/ffbase
FFOS := $(ROOT)/ffos
FFPACK := $(ROOT)/ffpack
FF := $(ROOT)/ff

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
PICLIB3 := $(PROJDIR)/piclib3/_$(OSFULL)-amd64
CRYPTOLIB3 := $(PROJDIR)/cryptolib3/_$(OSFULL)-amd64

FF_OBJ_DIR := ./ff-obj
ifeq ($(OPT),0)
	CFLAGS += -DFF_DEBUG -Werror
endif
# CFLAGS += -fsanitize=address
# LDFLAGS += -fsanitize=address -ldl
CFLAGS += -DFFBASE_HAVE_FFERR_STR -Wno-maybe-uninitialized
FFOS_CFLAGS := $(CFLAGS)
FF_CFLAGS := $(CFLAGS)

CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-stringop-overflow \
	-I$(SRCDIR) \
	-I$(PICLIB3)/.. \
	-I$(CRYPTOLIB3)/.. \
	-I$(FFBASE) -I$(FFPACK) -I$(FF) -I$(FFOS)

LDFLAGS += -Wno-stringop-overflow \
	-L$(FFPACK)/zlib -L$(FFPACK)/lzma -L$(FFPACK)/zstd \
	-L$(PICLIB3) \
	-L$(CRYPTOLIB3)

include $(PROJDIR)/makerules


package:
	rm -f $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) \
		&&  $(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH)-debug.$(PACK_EXT) ./*.debug
