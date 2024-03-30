# fcom modules

3PT_DIR := $(FCOM_DIR)/3pt/_$(OS)-$(CPU)

MODS += \
	crypto.$(SO) \
	hex.$(SO) \
	md5.$(SO)

crypto.$(SO): \
		aes.o \
		sha256.o \
		$(3PT_DIR)/SHA256.a \
		$(3PT_DIR)/AES.a
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

md5.$(SO): md5.o \
		$(3PT_DIR)/MD5.a
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

%.o: $(FCOM_DIR)/src/ops/%.c
	$(C) $(CFLAGS) $< -o $@
