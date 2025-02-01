# fcom: Windows-only modules

MODS += \
	reg.$(SO)

%.o: $(FCOM_DIR)/src/windows/%.c
	$(C) $(CFLAGS) $< -o $@
