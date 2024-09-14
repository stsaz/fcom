# fcom: pack

FFPACK_DIR := $(ROOT)/ffpack
FFPACK_BINDIR := $(FFPACK_DIR)/_$(OS)-$(CPU)

MODS += \
	7z.$(SO) \
	gz.$(SO) \
	iso.$(SO) \
	pack.$(SO) \
	tar.$(SO) \
	unpack.$(SO) \
	xz.$(SO) \
	zip.$(SO) \
	zst.$(SO)

LIBS3 += $(FFPACK_BINDIR)/liblzma-ffpack.$(SO) \
	$(FFPACK_BINDIR)/libz-ffpack.$(SO) \
	$(FFPACK_BINDIR)/libzstd-ffpack.$(SO)

%.o: $(FCOM_DIR)/src/pack/%.c
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

zip.o: $(FCOM_DIR)/src/pack/zip.c
	$(C) $(CFLAGS) -I$(FFPACK_DIR) -DFFPACK_ZIPREAD_ZLIB -DFFPACK_ZIPREAD_ZSTD -DFFPACK_ZIPWRITE_ZLIB -DFFPACK_ZIPWRITE_ZSTD $< -o $@
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
