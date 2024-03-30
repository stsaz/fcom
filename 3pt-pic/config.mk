include ../../ffbase/conf.mk

CFLAGS := -fpic -fvisibility=hidden -g
ifneq "$(DEBUG)" "1"
	CFLAGS += -O3
endif
CXXFLAGS := $(CFLAGS)

LINKFLAGS = -fpic $(LINK_INSTALLNAME_LOADERPATH) -lm
ifneq "$(DEBUG)" "1"
	LINKFLAGS += -s
endif
ifeq "$(COMPILER)" "gcc"
	LINKFLAGS += -static-libgcc
endif

# Set utils
CURL := curl -L
UNTAR_BZ2 := tar xjf
UNTAR_GZ := tar xzf
UNTAR_XZ := tar xJf
UNTAR_ZST := tar -x --zstd -f
UNZIP := unzip
