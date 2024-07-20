# libjpeg-turbo

include ../config.mk

VER := 2.1.5.1
URL := https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$(VER).tar.gz
PKG := $(FCOM_3PT)/jpeg-turbo/$(notdir $(URL))
DIR := libjpeg-turbo-$(VER)
LIB := libjpeg-turbo-ff.$(SO)

default: $(DIR)
	$(MKDIR) $(DIR)/build
	cd $(DIR)/build && cmake -G"Unix Makefiles" -DWITH_JPEG8=1 ..
	$(SED) 's/#define THREAD_LOCAL  __thread/#define THREAD_LOCAL/' $(DIR)/build/jconfigint.h
	$(SUBMAKE) $(LIB)

# download
$(PKG):
	$(CURL) -o $(PKG) $(URL)

# unpack
$(DIR): $(PKG)
	$(UNTAR_GZ) $(PKG)
	touch $(DIR)

# build
CFLAGS += \
	-I$(DIR)/build -I$(DIR)

SRC_C := \
	$(DIR)/jcapimin.c \
	$(DIR)/jcapistd.c \
	$(DIR)/jccoefct.c \
	$(DIR)/jccolor.c \
	$(DIR)/jcdctmgr.c \
	$(DIR)/jchuff.c \
	$(DIR)/jcicc.c \
	$(DIR)/jcinit.c \
	$(DIR)/jcmainct.c \
	$(DIR)/jcmarker.c \
	$(DIR)/jcmaster.c \
	$(DIR)/jcomapi.c \
	$(DIR)/jcparam.c \
	$(DIR)/jcphuff.c \
	$(DIR)/jcprepct.c \
	$(DIR)/jcsample.c \
	$(DIR)/jctrans.c \
	$(DIR)/jdapimin.c \
	$(DIR)/jdapistd.c \
	$(DIR)/jdatadst.c \
	$(DIR)/jdatasrc.c \
	$(DIR)/jdcoefct.c \
	$(DIR)/jdcolor.c \
	$(DIR)/jddctmgr.c \
	$(DIR)/jdhuff.c \
	$(DIR)/jdicc.c \
	$(DIR)/jdinput.c \
	$(DIR)/jdmainct.c \
	$(DIR)/jdmarker.c \
	$(DIR)/jdmaster.c \
	$(DIR)/jdmerge.c \
	$(DIR)/jdphuff.c \
	$(DIR)/jdpostct.c \
	$(DIR)/jdsample.c \
	$(DIR)/jdtrans.c \
	$(DIR)/jerror.c \
	$(DIR)/jfdctflt.c \
	$(DIR)/jfdctfst.c \
	$(DIR)/jfdctint.c \
	$(DIR)/jidctflt.c \
	$(DIR)/jidctfst.c \
	$(DIR)/jidctint.c \
	$(DIR)/jidctred.c \
	$(DIR)/jquant1.c \
	$(DIR)/jquant2.c \
	$(DIR)/jutils.c \
	$(DIR)/jmemmgr.c \
	$(DIR)/jmemnobs.c \
	$(DIR)/jaricom.c \
	$(DIR)/jcarith.c \
	$(DIR)/jdarith.c \
	$(DIR)/simd/x86_64/jsimd.c

SRC_ASM := \
	$(DIR)/simd/x86_64/jsimdcpu.asm \
	$(DIR)/simd/x86_64/jfdctflt-sse.asm \
	$(DIR)/simd/x86_64/jccolor-sse2.asm \
	$(DIR)/simd/x86_64/jcgray-sse2.asm \
	$(DIR)/simd/x86_64/jchuff-sse2.asm \
	$(DIR)/simd/x86_64/jcphuff-sse2.asm \
	$(DIR)/simd/x86_64/jcsample-sse2.asm \
	$(DIR)/simd/x86_64/jdcolor-sse2.asm \
	$(DIR)/simd/x86_64/jdmerge-sse2.asm \
	$(DIR)/simd/x86_64/jdsample-sse2.asm \
	$(DIR)/simd/x86_64/jfdctfst-sse2.asm \
	$(DIR)/simd/x86_64/jfdctint-sse2.asm \
	$(DIR)/simd/x86_64/jidctflt-sse2.asm \
	$(DIR)/simd/x86_64/jidctfst-sse2.asm \
	$(DIR)/simd/x86_64/jidctint-sse2.asm \
	$(DIR)/simd/x86_64/jidctred-sse2.asm \
	$(DIR)/simd/x86_64/jquantf-sse2.asm \
	$(DIR)/simd/x86_64/jquanti-sse2.asm \
	$(DIR)/simd/x86_64/jccolor-avx2.asm \
	$(DIR)/simd/x86_64/jcgray-avx2.asm \
	$(DIR)/simd/x86_64/jcsample-avx2.asm \
	$(DIR)/simd/x86_64/jdcolor-avx2.asm \
	$(DIR)/simd/x86_64/jdmerge-avx2.asm \
	$(DIR)/simd/x86_64/jdsample-avx2.asm \
	$(DIR)/simd/x86_64/jfdctint-avx2.asm \
	$(DIR)/simd/x86_64/jidctint-avx2.asm \
	$(DIR)/simd/x86_64/jquanti-avx2.asm

OBJ := $(SRC_ASM:.asm=.asm.o) jpeg-ff.o $(SRC_C:.c=.o)

%.o: $(FCOM_3PT)/jpeg-turbo/%.c
	$(C) $(CFLAGS) $< -o $@

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

NASMFLAGS := -I$(DIR)/simd/nasm -I$(DIR)/simd/x86_64 -I$(DIR) \
	-D__x86_64__ -DPIC
ifeq ($(OS),windows)
	NASMFLAGS += -DWIN64 -f win64
else
	NASMFLAGS += -DELF -f elf64
endif

%.asm.o: %.asm
	nasm $(NASMFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

clean:
	$(RM) $(OBJ) $(DIR)
