# fcom makefile

ROOT := ..
FCOM_DIR := $(ROOT)/fcom
FFBASE_DIR := $(ROOT)/ffbase
FFSYS_DIR := $(ROOT)/ffsys

include $(FFBASE_DIR)/conf.mk

EXE := fcom$(DOTEXE)

CFLAGS := -MMD -MP \
	-I$(FCOM_DIR)/src -I$(FFSYS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR -DFFBASE_MEM_ASSERT \
	-Wall -Wextra -Wno-unused-parameter -Wno-multichar \
	-fPIC \
	-g
ifeq "$(CPU)" "amd64"
	CFLAGS += -march=nehalem
endif
ifeq "$(DEBUG)" "1"
	CFLAGS += -O0 -DFF_DEBUG -Werror
else
	CFLAGS += -O3 -fno-strict-aliasing -fvisibility=hidden
endif
ifeq "$(ASAN)" "1"
	CFLAGS += -fsanitize=address
	LINKFLAGS += -fsanitize=address
endif
CXXFLAGS := -std=c++11 $(CFLAGS)
CFLAGS := -std=c99 $(CFLAGS)
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

default: build
ifneq "$(DEBUG)" "1"
	$(SUBMAKE) strip-debug
endif
	$(SUBMAKE) app

-include $(wildcard *.d)

include $(FCOM_DIR)/src/core/Makefile
include $(FCOM_DIR)/src/exe/Makefile
include $(FCOM_DIR)/src/fs/Makefile
include $(FCOM_DIR)/src/ops/Makefile
include $(FCOM_DIR)/src/pack/Makefile
include $(FCOM_DIR)/src/pic/Makefile
include $(FCOM_DIR)/src/text/Makefile
ifeq "$(OS)" "windows"
include $(FCOM_DIR)/src/windows/Makefile
endif

ifeq "$(TARGETS)" ""
override TARGETS := core.$(SO) $(EXE) $(MODS)
endif
build: $(TARGETS)

clean:
	$(RM) $(MODS) *.o


%.$(SO): %.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@


strip-debug: $(addsuffix .debug,$(TARGETS))
%.debug: %
	$(OBJCOPY) --only-keep-debug $< $@
	$(STRIP) $<
	$(OBJCOPY) --add-gnu-debuglink=$@ $<
	touch $@


# copy files to app directory
APP_DIR := fcom-1
app:
	$(MKDIR) $(APP_DIR)
	$(CP) \
		$(EXE) \
		core.$(SO) \
		$(FCOM_DIR)/README.md \
		$(FCOM_DIR)/help.txt \
		$(APP_DIR)
	chmod 0755 $(APP_DIR) $(APP_DIR)/$(EXE)
	chmod 0644 $(APP_DIR)/README.* $(APP_DIR)/*.$(SO)

	$(MKDIR) $(APP_DIR)/ops
	chmod 0755 $(APP_DIR)/ops
	$(CP) $(MODS) $(APP_DIR)/ops
ifneq "$(LIBS3)" ""
	$(CP) $(LIBS3) $(APP_DIR)/ops
endif
	chmod 0644 $(APP_DIR)/ops/*.$(SO)

	$(SUBMAKE) app-gsync


# package
PKG_VER := test
PKG_ARCH := $(CPU)
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif

PKG_NAME := fcom-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT)
$(PKG_NAME): $(APP_DIR)
	$(PKG_PACKER) $@ $<
package: $(PKG_NAME)

PKG_DEBUG_NAME := fcom-$(PKG_VER)-$(OS)-$(PKG_ARCH)-debug.$(PKG_EXT)
$(PKG_DEBUG_NAME):
	$(PKG_PACKER) $@ *.debug
package-debug: $(PKG_DEBUG_NAME)

release: default
	$(SUBMAKE) package
	$(SUBMAKE) package-debug
