# fcom

PROJ := fcom
ROOT := ..
PROJDIR := $(ROOT)/fcom
SRCDIR := $(PROJDIR)/src
OPT := LTO3
FFBASE := $(ROOT)/ffbase
FFOS := $(ROOT)/ffos
FFPACK := $(ROOT)/ffpack

include $(FFOS)/makeconf
LINK := $(LD)
LINKFLAGS := $(LDFLAGS)

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

ifeq ($(OPT),0)
	CFLAGS += -DFF_DEBUG -Werror
endif
# CFLAGS += -fsanitize=address
# LINKFLAGS += -fsanitize=address -ldl
CFLAGS += -DFFBASE_HAVE_FFERR_STR -Wno-maybe-uninitialized

CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-stringop-overflow \
	-I$(SRCDIR) \
	-I$(PICLIB3)/.. \
	-I$(CRYPTOLIB3)/.. \
	-I$(FFBASE) -I$(FFPACK) -I$(FFOS)

LINKFLAGS += -Wno-stringop-overflow \
	-L$(FFPACK)/zlib -L$(FFPACK)/lzma -L$(FFPACK)/zstd \
	-L$(PICLIB3) \
	-L$(CRYPTOLIB3)


OBJ_DIR := .
BINS := $(BIN) core.$(SO) file.$(SO) fsync.$(SO) arc.$(SO) pic.$(SO) net.$(SO) crypto.$(SO) gui.$(SO)

ifeq ($(OS),win)
RES := $(OBJ_DIR)/fcom.coff
BINS += $(RES)
endif

all: $(BINS)

FF_O := $(OBJ_DIR)/ffos.o
ifeq "$(OSFULL)" "windows"
	FF_O +=	$(OBJ_DIR)/ffwin.o
	FFOS_SKT := $(OBJ_DIR)/ffwin-skt.o
else ifeq "$(OSFULL)" "macos"
	FF_O +=	$(OBJ_DIR)/ffapple.o
else ifeq "$(OSFULL)" "freebsd"
	FF_O +=	$(OBJ_DIR)/ffbsd.o
else
	FF_O +=	$(OBJ_DIR)/ffunix.o $(OBJ_DIR)/fflinux.o
endif
GLOB_HDRS := $(SRCDIR)/fcom.h \
	$(wildcard $(SRCDIR)/util/*.h) $(wildcard $(SRCDIR)/util/ffos-compat/*.h) \
	$(wildcard $(FFBASE)/ffbase/*.h) $(wildcard $(FFOS)/FFOS/*.h)

$(OBJ_DIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/*.h
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/core/%.c $(SRCDIR)/core/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/arc/%.c $(SRCDIR)/arc/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/fileops/%.c $(SRCDIR)/fileops/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/pic/%.c $(SRCDIR)/pic/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/pic/util/%.c $(SRCDIR)/pic/util/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/text/%.c $(SRCDIR)/text/*.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(FFPACK)/crc/%.c
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/util/%.c
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/util/%.c
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/util/ffos-compat/%.c
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(CRYPTOLIB3)/../sha1/%.c
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(CRYPTOLIB3)/../sha/%.c
	$(C) $(CFLAGS) $< -o $@

$(RES): $(PROJDIR)/res/fcom.rc $(PROJDIR)/res/fcom.exe.manifest $(PROJDIR)/res/fcom.ico
	$(WINDRES) -I$(SRCDIR) -I$(FFBASE) -I$(FFOS) $(PROJDIR)/res/fcom.rc $@


BIN_O := $(OBJ_DIR)/main.o \
	$(OBJ_DIR)/fftime.o \
	$(FF_O)

ifeq ($(OS),win)
BIN_O += $(RES)
endif

$(BIN): $(BIN_O)
	$(LINK) $(BIN_O) $(LINKFLAGS) $(LD_LDL) -o $@


CORE_O := \
	$(OBJ_DIR)/core.o \
	$(OBJ_DIR)/com.o \
	$(OBJ_DIR)/file.o \
	$(OBJ_DIR)/file-std.o \
	$(OBJ_DIR)/crc.o \
	$(FF_O) \
	$(FFOS_THD) \
	$(OBJ_DIR)/ffpath.o \
	$(OBJ_DIR)/fftime.o \
	$(OBJ_DIR)/fffileread.o \
	$(OBJ_DIR)/fffilewrite.o \
	$(OBJ_DIR)/ffthpool.o

core.$(SO): $(CORE_O)
	$(LINK) -shared $(CORE_O) $(LINKFLAGS) $(LD_LDL) -o $@


FILE_O := $(OBJ_DIR)/fop.o \
	$(OBJ_DIR)/text.o \
	$(OBJ_DIR)/crc.o \
	$(OBJ_DIR)/ffpe.o \
	$(OBJ_DIR)/ffpath.o \
	$(FF_O)

ifeq ($(OS),win)
	FILE_O += $(OBJ_DIR)/sys-win.o \
		$(OBJ_DIR)/fftime.o \
		$(OBJ_DIR)/ffwreg.o
	FILE_LINKFLAGS := -lole32 -luuid
endif

file.$(SO): $(FILE_O)
	$(LINK) -shared $(FILE_O) $(LINKFLAGS) $(FILE_LINKFLAGS) -o $@


FSYNC_O := $(OBJ_DIR)/fsync.o $(OBJ_DIR)/snapshot.o \
	$(OBJ_DIR)/crc.o \
	$(OBJ_DIR)/ffrbtree.o \
	$(OBJ_DIR)/fftime.o \
	$(OBJ_DIR)/ffpath.o \
	$(FF_O)
$(OBJ_DIR)/%.o: $(SRCDIR)/fsync/%.c $(SRCDIR)/fsync/fsync.h $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
fsync.$(SO): $(FSYNC_O)
	$(LINK) -shared $(FSYNC_O) $(LINKFLAGS) -o $@


crypto.$(SO): $(OBJ_DIR)/crypto.o \
	$(OBJ_DIR)/ffpath.o \
	$(OBJ_DIR)/crc.o $(FF_O) \
	$(OBJ_DIR)/sha256.o $(OBJ_DIR)/sha256-ff.o $(OBJ_DIR)/sha1.o \
	$(CRYPTOLIB3)/AES-ff.a

	$(LINK) -shared $+ $(LINKFLAGS) -o $@


ARC_O := $(OBJ_DIR)/arc.o \
	$(OBJ_DIR)/zstd.o \
	$(OBJ_DIR)/gz.o \
	$(OBJ_DIR)/xz.o \
	$(OBJ_DIR)/zip.o \
	$(OBJ_DIR)/tar.o \
	$(OBJ_DIR)/7z.o \
	$(OBJ_DIR)/iso.o \
	$(OBJ_DIR)/ico.o \
	$(OBJ_DIR)/crc.o \
	$(OBJ_DIR)/fftime.o \
	$(OBJ_DIR)/ffpath.o \
	$(OBJ_DIR)/ffico.o \
	$(FF_O)
arc.$(SO): $(ARC_O)
	$(LINK) -shared $(ARC_O) $(LINKFLAGS) $(LD_RPATH_ORIGIN) -lz-ff -llzma-ff -lzstd-ffpack -o $@


PIC_O := $(OBJ_DIR)/pic.o \
	$(OBJ_DIR)/bmp.o \
	$(OBJ_DIR)/png.o \
	$(OBJ_DIR)/jpg.o \
	$(OBJ_DIR)/ffpic.o \
	$(OBJ_DIR)/ffbmp.o \
	$(OBJ_DIR)/ffjpeg.o \
	$(OBJ_DIR)/ffpng.o \
	$(OBJ_DIR)/ffpath.o \
	$(FF_O)
pic.$(SO): $(PIC_O)
	$(LINK) -shared $(PIC_O) $(LINKFLAGS) $(LD_RPATH_ORIGIN) -ljpeg-turbo-ff -lpng-ff -o $@


NET_O := $(OBJ_DIR)/dns.o \
	$(OBJ_DIR)/ffdns-client.o \
	$(OBJ_DIR)/ffcrc.o \
	$(FF_O) \
	$(FFOS_SKT)
net.$(SO): $(NET_O)
	$(LINK) -shared $(NET_O) $(LINKFLAGS) $(LD_RPATH_ORIGIN) $(LD_LWS2_32) -o $@


ifeq ($(OS),win)
$(OBJ_DIR)/%.o: $(SRCDIR)/gui-winapi/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/gui-fsync/%.c $(GLOB_HDRS) $(wildcard $(SRCDIR)/gui-fsync/*.h)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/gui-scrshots/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/util/gui-winapi/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $< -o $@
GUI_O := $(OBJ_DIR)/gui.o \
	$(OBJ_DIR)/scrshots.o \
	$(OBJ_DIR)/gsync.o \
	$(OBJ_DIR)/ffgui-winapi-loader.o \
	$(OBJ_DIR)/ffgui-winapi.o \
	$(OBJ_DIR)/fftime.o \
	$(OBJ_DIR)/ffpath.o \
	$(FF_O)
gui.$(SO): $(GUI_O)
	$(LINK) -shared $(GUI_O) $(LINKFLAGS) -lshell32 -luxtheme -lcomctl32 -lcomdlg32 -lgdi32 -lole32 -luuid -o $@
endif

ifeq ($(OS),linux)
CFLAGS_GTK := -I/usr/include/gtk-3.0 -I/usr/include/pango-1.0 -I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include -I/usr/include/fribidi -I/usr/include/cairo -I/usr/include/pixman-1 -I/usr/include/freetype2 -I/usr/include/libpng16 -I/usr/include/uuid -I/usr/include/harfbuzz -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/gio-unix-2.0/ -I/usr/include/libdrm -I/usr/include/valgrind -I/usr/include/atk-1.0 -I/usr/include/at-spi2-atk/2.0 -I/usr/include/at-spi-2.0 -I/usr/include/dbus-1.0 -I/usr/lib64/dbus-1.0/include
LIBS_GTK := -lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -lfribidi -latk-1.0 -lcairo-gobject -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0
$(OBJ_DIR)/%.o: $(SRCDIR)/gui-gtk/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $(CFLAGS_GTK) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/gui-fsync-gtk/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $(CFLAGS_GTK) $< -o $@
$(OBJ_DIR)/%.o: $(SRCDIR)/util/gui-gtk/%.c $(GLOB_HDRS)
	$(C) $(CFLAGS) $(CFLAGS_GTK) $< -o $@
GUI_O := $(OBJ_DIR)/gui-gtk.o \
	$(OBJ_DIR)/gsync.o \
	$(OBJ_DIR)/ffgui-gtk-loader.o \
	$(OBJ_DIR)/ffgui-gtk.o \
	$(OBJ_DIR)/ffpath.o \
	$(FF_O)
gui.$(SO): $(GUI_O)
	$(LINK) -shared $(GUI_O) $(LINKFLAGS) $(LIBS_GTK) -pthread -o $@
endif

TEST_O := $(OBJ_DIR)/tester.o \
	$(OBJ_DIR)/com.o \
	$(OBJ_DIR)/fftest.o \
	$(FF_O)
TESTER := tester
ifeq ($(OS),win)
TESTER := tester.exe
endif
$(TESTER): $(TEST_O)
	$(LINK) $(TEST_O) $(LINKFLAGS) $(LD_LDL) -o $@


clean:
	rm -vf $(BINS) *.debug *.o $(RES)

distclean: clean ffclean
	rm -vfr $(INSTDIR) ./$(PROJ)-*.zip ./$(PROJ)-*.tar.xz

strip: $(BINS:.$(SO)=.$(SO).debug) $(BIN).debug $(BINS:.exe=.exe.debug)

copy-bins:
	$(CP) \
		*.$(SO) \
		$(INSTDIR)/mod

install-only:
	mkdir -vp $(INSTDIR)
	$(CP) \
		$(PROJDIR)/fcom.conf \
		$(PROJDIR)/help.txt $(PROJDIR)/CHANGES.txt \
		$(INSTDIR)/
	$(CP) $(PROJDIR)/README.md $(INSTDIR)/README.txt

ifeq ($(OS),win)
	$(CP) $(PROJDIR)/src/gui-fsync/*.gui $(PROJDIR)/src/gui-scrshots/*.gui $(PROJDIR)/res/screenshots.ico $(INSTDIR)
	unix2dos $(INSTDIR)/*.txt $(INSTDIR)/*.conf $(INSTDIR)/*.gui
else
	$(CP) $(PROJDIR)/src/gui-fsync-gtk/*.gui $(INSTDIR)
endif

	$(CP) $(BIN) $(INSTDIR)/
	mkdir -vp $(INSTDIR)/mod
	$(CP) \
		*.$(SO) \
		$(FFPACK)/zlib/libz-ff.$(SO) $(FFPACK)/lzma/liblzma-ff.$(SO) \
		$(FFPACK)/zstd/libzstd-ffpack.$(SO) \
		$(PICLIB3)/libjpeg-turbo-ff.$(SO) $(PICLIB3)/libpng-ff.$(SO) \
		$(INSTDIR)/mod

ifneq ($(OS),win)
	chmod 644 $(INSTDIR)/*.txt $(INSTDIR)/*.conf
	chmod 755 $(INSTDIR)/$(BIN)
	chmod 755 $(INSTDIR)/mod
	chmod 644 $(INSTDIR)/mod/*
endif

installd: all
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) install-only

install: all
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) strip
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) install-only

package: $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH).$(PACK_EXT) $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH)-debug.$(PACK_EXT) ./*.debug

install-package: install
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) package
