# fcom: Windows-only modules

MODS += \
	listdisk.$(SO) \
	mount.$(SO) \
	reg.$(SO)

%.o: $(FCOM_DIR)/src/windows/%.c
	$(C) $(CFLAGS) $< -o $@
