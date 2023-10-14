# fcom: file system modules

MODS := \
	copy.$(SO) \
	list.$(SO) \
	move.$(SO) \
	sync.$(SO) \
	touch.$(SO) \
	trash.$(SO)

%.o: $(FCOM_DIR)/src/fs/%.c
	$(C) $(CFLAGS) $< -o $@
