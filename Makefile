# fcom makefile

ROOT := ..
FCOM_DIR := $(ROOT)/fcom
FFBASE_DIR := $(ROOT)/ffbase
FFOS_DIR := $(ROOT)/ffos
FFPACK_DIR := $(ROOT)/ffpack
AVPACK_DIR := $(ROOT)/avpack

include $(FFBASE_DIR)/test/makeconf

BIN := fcom$(DOTEXE)
MODS := \
	copy.$(SO) \
	crypto.$(SO) \
	hex.$(SO) \
	list.$(SO) \
	md5.$(SO) \
	move.$(SO) \
	pic.$(SO) \
	sync.$(SO) \
	textcount.$(SO) \
	touch.$(SO) \
	trash.$(SO) \
	ico-extract.$(SO) \
	utf8.$(SO)

ifeq "$(OS)" "windows"
MODS += \
	listdisk.$(SO) \
	mount.$(SO) \
	reg.$(SO)
endif

MODS += \
	7z.$(SO) \
	gz.$(SO) \
	iso.$(SO) \
	tar.$(SO) \
	unpack.$(SO) \
	xz.$(SO) \
	zip.$(SO) \
	zst.$(SO)

BINS := $(BIN) core.$(SO) $(MODS)

CFLAGS := -I$(FCOM_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR -DFFBASE_MEM_ASSERT \
	-Wall -Wextra -Wno-unused-parameter -Wno-multichar \
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
3PT_PIC_DIR := $(FCOM_DIR)/3pt-pic/_$(OS)-$(CPU)
FFPACK_BINDIR := $(FFPACK_DIR)/_$(OS)-$(CPU)

GDEPS := $(FCOM_DIR)/src/fcom.h \
	$(wildcard $(FCOM_DIR)/src/util/*.h)

# build, install
default: $(BINS)
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) app

# build, install, package
build-package: default
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) package

clean:
	$(RM) $(MODS) *.o

%.o: $(FCOM_DIR)/src/%.c \
		$(wildcard $(FCOM_DIR)/src/*.h)
	$(C) $(CFLAGS) $< -o $@
$(BIN): main.o args.o
	$(LINK) $+ $(LINKFLAGS) -o $@

%.o: $(FCOM_DIR)/src/core/%.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/core/*.h)
	$(C) $(CFLAGS) $< -o $@
core.$(SO): com.o core.o file.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

%.o: $(FCOM_DIR)/src/ops/%.c $(GDEPS)
	$(C) $(CFLAGS) $< -o $@

copy.o: $(FCOM_DIR)/src/ops/copy.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/copy-*.h)
	$(C) $(CFLAGS) $< -o $@

sync.o: $(FCOM_DIR)/src/ops/sync.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/sync-*.h)
	$(C) $(CFLAGS) $< -o $@

md5.o: $(FCOM_DIR)/src/ops/md5.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/md5-*.h)
	$(C) $(CFLAGS) $< -o $@

reg.o: $(FCOM_DIR)/src/ops/reg.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/ops/reg-*.h)
	$(C) $(CFLAGS) $< -o $@

%.$(SO): %.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

crypto.$(SO): \
		aes.o \
		sha256.o \
		$(3PT_DIR)/SHA256.a \
		$(3PT_DIR)/AES.a
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

md5.$(SO): md5.o \
		$(3PT_DIR)/MD5.a
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

%.o: $(FCOM_DIR)/src/pic/%.c $(GDEPS) \
		$(wildcard $(FCOM_DIR)/src/pic/*.h)
	$(C) $(CFLAGS) -I$(AVPACK_DIR) $< -o $@
pic.$(SO): pic.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(3PT_PIC_DIR) -ljpeg-turbo-ff -lpng-ff $(LINK_RPATH_ORIGIN) -o $@

# PACK

%.o: $(FCOM_DIR)/src/pack/%.c $(GDEPS) \
		$(wildcard $(FFPACK_DIR)/ffpack/*.h)
	$(C) $(CFLAGS) -I$(FFPACK_DIR) $< -o $@

tar.$(SO): tar.o untar.o \
		crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) $(LINK_RPATH_ORIGIN) -o $@

gz.$(SO): gz.o ungz.o \
		crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -lz-ffpack $(LINK_RPATH_ORIGIN) -o $@

xz.$(SO): unxz.o \
		crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -llzma-ffpack $(LINK_RPATH_ORIGIN) -o $@

zip.o: $(FCOM_DIR)/src/pack/zip.c $(GDEPS) \
		$(wildcard $(FFPACK_DIR)/ffpack/*.h)
	$(C) $(CFLAGS) -I$(FFPACK_DIR) -DFFPACK_ZIPWRITE_ZLIB -DFFPACK_ZIPWRITE_ZSTD $< -o $@
crc.o: $(FFPACK_DIR)/crc/crc.c
	$(C) $(CFLAGS) -I$(FFPACK_DIR) $< -o $@
zip.$(SO): zip.o unzip.o \
		crc32.o crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -lzstd-ffpack -lz-ffpack $(LINK_RPATH_ORIGIN) -o $@

zst.$(SO): zst.o unzst.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -lzstd-ffpack $(LINK_RPATH_ORIGIN) -o $@

7z.$(SO): un7z.o \
		crc.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(FFPACK_BINDIR) -lz-ffpack -llzma-ffpack $(LINK_RPATH_ORIGIN) -o $@

iso.$(SO): iso.o uniso.o
	$(LINK) -shared $+ $(LINKFLAGS) $(LINK_RPATH_ORIGIN) -o $@


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
		$(FFPACK_BINDIR)/liblzma-ffpack.$(SO) \
		$(FFPACK_BINDIR)/libz-ffpack.$(SO) \
		$(FFPACK_BINDIR)/libzstd-ffpack.$(SO) \
		fcom-1/ops
	$(CP) \
		$(3PT_PIC_DIR)/libjpeg-turbo-ff.$(SO) \
		$(3PT_PIC_DIR)/libpng-ff.$(SO) \
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
