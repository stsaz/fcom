# fcom makefile

ROOT := ..
FCOM_DIR := $(ROOT)/fcom/v1
FFBASE_DIR := $(ROOT)/ffbase
FFOS_DIR := $(ROOT)/ffos
FFPACK_DIR := $(ROOT)/ffpack
CPU := amd64

include $(FFBASE_DIR)/test/makeconf

BIN := fcom$(DOTEXE)
MODS := \
	copy.$(SO) \
	crypto.$(SO) \
	hex.$(SO) \
	list.$(SO) \
	move.$(SO) \
	sync.$(SO) \
	textcount.$(SO) \
	touch.$(SO) \
	trash.$(SO) \
	zip.$(SO)
BINS := $(BIN) core.$(SO) $(MODS)

CFLAGS := -I$(FCOM_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR -DFFBASE_MEM_ASSERT \
	-Wall -Wextra -Wno-unused-parameter \
	-fPIC \
	-march=nehalem
ifeq "$(DEBUG)" "1"
	CFLAGS += -g -O0 -DFF_DEBUG -Werror
# 	CFLAGS += -fsanitize=address
# 	LINKFLAGS += -fsanitize=address
else
	CFLAGS += -O3 -fno-strict-aliasing -fvisibility=hidden
	LINKFLAGS += -s
endif

3PT_DIR := $(FCOM_DIR)/3pt/_$(OS)-$(CPU)
FFPACK_BINDIR := $(FFPACK_DIR)/_$(OS)-$(CPU)

GDEPS := $(FCOM_DIR)/src/fcom.h \
	$(wildcard $(FCOM_DIR)/src/util/*.h)

# build, install
default: $(BINS)
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) app

# build, install, package
build-package: default
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) package

%.o: $(FCOM_DIR)/src/%.c \
		$(wildcard $(FCOM_DIR)/src/*.h)
	$(C) $(CFLAGS) $< -o $@
$(BIN): main.o args.o
	$(LINK) $+ $(LINKFLAGS) -o $@

# Core
%.o: $(FCOM_DIR)/src/core/%.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/core/*.h)
	$(C) $(CFLAGS) $< -o $@
core.$(SO): com.o core.o file.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

# ops
%.o: $(FCOM_DIR)/src/ops/%.c $(GDEPS)
	$(C) $(CFLAGS) $< -o $@

copy.o: $(FCOM_DIR)/src/ops/copy.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/copy-*.h)
	$(C) $(CFLAGS) $< -o $@

sync.o: $(FCOM_DIR)/src/ops/sync.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/sync-*.h)
	$(C) $(CFLAGS) $< -o $@

# mod.copy, sync
%.$(SO): %.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

crypto.$(SO): \
		aes.o \
		md5.o \
		sha256.o \
		$(3PT_DIR)/MD5.a \
		$(3PT_DIR)/SHA256.a \
		$(3PT_DIR)/AES.a
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

zip.o: $(FCOM_DIR)/src/ops/zip.c $(GDEPS) \
		$(wildcard $(FFPACK_DIR)/ffpack/*.h)
	$(C) $(CFLAGS) -I$(FFPACK_DIR) -DFFPACK_ZIPWRITE_ZLIB -DFFPACK_ZIPWRITE_ZSTD $< -o $@
unzip.o: $(FCOM_DIR)/src/ops/unzip.c $(GDEPS) \
		$(wildcard $(FFPACK_DIR)/ffpack/*.h)
	$(C) $(CFLAGS) -I$(FFPACK_DIR) $< -o $@
crc.o: $(FFPACK_DIR)/crc/crc.c
	$(C) $(CFLAGS) -I$(FFPACK_DIR) $< -o $@
zip.$(SO): zip.o \
		unzip.o \
		crc32.o crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -lzstd-ffpack -lz-ffpack $(LINK_RPATH_ORIGIN) -o $@

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@


# copy files to app directory
app:
	$(MKDIR) fcom-1
	$(CP) \
		$(BIN) \
		core.$(SO) \
		$(FCOM_DIR)/README.md \
		$(FCOM_DIR)/help.txt \
		fcom-1
	chmod 0755 fcom-1 fcom-1/$(BIN)
	chmod 0644 fcom-1/README.* fcom-1/*.$(SO)

	$(MKDIR) fcom-1/ops
	chmod 0755 fcom-1/ops
	$(CP) $(MODS) fcom-1/ops
	$(CP) \
		$(FFPACK_BINDIR)/libzstd-ffpack.$(SO) \
		$(FFPACK_BINDIR)/libz-ffpack.$(SO) \
		fcom-1/ops
	chmod 0644 fcom-1/ops/*.$(SO)


# package
PKG_VER := 0
PKG_ARCH := $(CPU)
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif
package:
	$(PKG_PACKER) fcom-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT) fcom-1
