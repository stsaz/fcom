# fcom makefile

ROOT := ..
FCOM_DIR := $(ROOT)/fcom/v1
FFBASE_DIR := $(ROOT)/ffbase
FFOS_DIR := $(ROOT)/ffos

include $(FFBASE_DIR)/test/makeconf

BIN := fcom$(DOTEXE)
MODS :=
BINS := $(BIN) core.$(SO) $(MODS)

CFLAGS := -I$(FCOM_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR \
	-Wall \
	-fPIC \
	-march=nehalem
LINKFLAGS :=
ifeq "$(OPT)" "0"
	CFLAGS += -g -O0 -DFF_DEBUG -Werror
else
	CFLAGS += -O3 -fno-strict-aliasing
	LINKFLAGS := -s
endif


# build, install
default: $(BINS)
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) install

# build, install, package
build-package: default
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) package

%.o: $(FCOM_DIR)/src/%.c \
		$(wildcard $(FCOM_DIR)/src/*.h) \
		$(wildcard $(FCOM_DIR)/src/util/*.h) \
		$(FCOM_DIR)/Makefile
	$(C) $(CFLAGS) $< -o $@
$(BIN): main.o args.o
	$(LINK) $+ $(LINKFLAGS) -o $@

# Core
%.o: $(FCOM_DIR)/src/core/%.c \
		$(wildcard $(FCOM_DIR)/src/core/*.h) \
		$(FCOM_DIR)/src/fcom.h \
		$(wildcard $(FCOM_DIR)/src/util/*.h) \
		$(FCOM_DIR)/Makefile
	$(C) $(CFLAGS) $< -o $@
core.$(SO): com.o core.o file.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@


# copy files to install directory
INST_DIR := fcom-1
install:
	$(MKDIR) $(INST_DIR)
	$(CP) \
		$(BIN) \
		core.$(SO) \
		$(FCOM_DIR)/README.md \
		$(FCOM_DIR)/help.txt \
		$(INST_DIR)
	mkdir -p $(INST_DIR)/ops
	$(CP) $(MODS) $(INST_DIR)/ops
	chmod 0755 $(INST_DIR) $(INST_DIR)/ops $(INST_DIR)/$(BIN)
	chmod 0644 $(INST_DIR)/README.* $(INST_DIR)/*.$(SO) $(INST_DIR)/ops/*.$(SO)


# package
PKG_VER :=
PKG_ARCH := amd64
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	PKG_ARCH := x64
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif
package:
	$(PKG_PACKER) fcom-v$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT) $(INST_DIR)
