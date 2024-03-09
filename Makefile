# fcom makefile

ROOT := ..
FCOM_DIR := $(ROOT)/fcom
FFBASE_DIR := $(ROOT)/ffbase
FFSYS_DIR := $(ROOT)/ffsys

include $(FFBASE_DIR)/conf.mk

SUBMAKE := $(MAKE) -f $(firstword $(MAKEFILE_LIST))
BIN := fcom$(DOTEXE)

CFLAGS := -MMD -MP \
	-I$(FCOM_DIR)/src -I$(FFSYS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR -DFFBASE_MEM_ASSERT \
	-Wall -Wextra -Wno-unused-parameter -Wno-multichar \
	-fPIC \
	-g
CFLAGS += -march=nehalem
ifeq "$(DEBUG)" "1"
	CFLAGS += -O0 -DFF_DEBUG -Werror
else
	CFLAGS += -O3 -fno-strict-aliasing -fvisibility=hidden
endif
ifeq "$(ASAN)" "1"
	CFLAGS += -fsanitize=address
	LINKFLAGS += -fsanitize=address
endif
CXXFLAGS := $(CFLAGS)
LINKXXFLAGS := $(LINKFLAGS) -static-libgcc
ifeq "$(OS)" "windows"
	# remove runtime dependency on both libstdc++-6.dll and libwinpthread-1.dll
	LINKXXFLAGS += -Wl,-Bstatic
else
	LINKXXFLAGS += -static-libstdc++
endif
LINK_DL :=
ifeq "$(OS)" "linux"
	LINK_DL := -ldl
endif

# build, install
ifneq "$(DEBUG)" "1"
default: strip-debug
	$(SUBMAKE) app
else
default: build
	$(SUBMAKE) app
endif

# build, install, package
build-package: default
	$(SUBMAKE) package

-include $(wildcard *.d)

include $(FCOM_DIR)/src/fs/Makefile
include $(FCOM_DIR)/src/ops/Makefile
include $(FCOM_DIR)/src/pack/Makefile
include $(FCOM_DIR)/src/pic/Makefile
include $(FCOM_DIR)/src/text/Makefile
ifeq "$(OS)" "windows"
include $(FCOM_DIR)/src/windows/Makefile
endif

build: $(BIN) core.$(SO) $(MODS)

clean:
	$(RM) $(MODS) *.o

%.o: $(FCOM_DIR)/src/exe/%.c
	$(C) $(CFLAGS) $< -o $@
$(BIN): main.o args.o \
		core.$(SO)
	$(LINK) $+ $(LINKFLAGS) $(LINK_RPATH_ORIGIN) -o $@

%.o: $(FCOM_DIR)/src/core/%.c
	$(C) $(CFLAGS) $< -o $@
core.$(SO): com.o core.o file.o
	$(LINK) -shared $+ $(LINKFLAGS) $(LINK_DL) -o $@

%.$(SO): %.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@


# Debug symbols
strip-debug: core.$(SO).debug \
		fcom$(DOTEXE).debug \
		$(EXES:.exe=.exe.debug) \
		$(MODS:.$(SO)=.$(SO).debug)
%.debug: %
	$(OBJCOPY) --only-keep-debug $< $@
	$(STRIP) $<
	$(OBJCOPY) --add-gnu-debuglink=$@ $<
	touch $@


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
	$(CP) $(LIBS3) fcom-1/ops
	chmod 0644 fcom-1/ops/*.$(SO)

	$(SUBMAKE) app-gsync


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
