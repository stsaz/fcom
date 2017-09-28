# fcom v0.1 makefile

PROJ := fcom
ROOT := ..
PROJDIR := $(ROOT)/fcom
SRCDIR := $(PROJDIR)/src
OPT := LTO3

FFOS := $(ROOT)/ffos
FF := $(ROOT)/ff
FF3PT := $(ROOT)/ff-3pt

include $(FFOS)/makeconf

ifeq ($(OS),win)
INSTDIR := ./$(PROJ)
BIN := fcom.exe
else
INSTDIR := ./$(PROJ)-0
BIN := fcom-bin
endif
VER :=

FF_OBJ_DIR := ./ff-obj
FFOS_CFLAGS := $(CFLAGS)
FF_CFLAGS := $(CFLAGS)
FF3PT_CFLAGS := $(CFLAGS)
FF3PTLIB := $(FF3PT)-bin/$(OS)-$(ARCH)

CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers \
	-I$(SRCDIR) -I$(FF) -I$(FFOS) -I$(FF3PT)

LDFLAGS += -L$(FF3PTLIB)

include $(PROJDIR)/makerules


package: install
	rm -f $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) \
		&&  $(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH)-debug.$(PACK_EXT) ./*.debug
