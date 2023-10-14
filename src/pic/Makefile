# fcom: pic modules

AVPACK_DIR := $(ROOT)/avpack
3PT_PIC_DIR := $(FCOM_DIR)/3pt-pic/_$(OS)-$(CPU)

MODS += \
	pic.$(SO) \
	ico-extract.$(SO)
LIBS3 += \
	$(3PT_PIC_DIR)/libjpeg-turbo-ff.$(SO) \
	$(3PT_PIC_DIR)/libpng-ff.$(SO)

%.o: $(FCOM_DIR)/src/pic/%.c
	$(C) $(CFLAGS) -I$(AVPACK_DIR) $< -o $@

pic.$(SO): pic.o
	$(LINK) -shared $+ $(LINKFLAGS) -L$(3PT_PIC_DIR) -ljpeg-turbo-ff -lpng-ff $(LINK_RPATH_ORIGIN) -o $@
