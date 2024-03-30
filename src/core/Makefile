# fcom: core

%.o: $(FCOM_DIR)/src/core/%.c
	$(C) $(CFLAGS) $< -o $@

core.$(SO): com.o core.o file.o
	$(LINK) -shared $+ $(LINKFLAGS) $(LINK_DL) -o $@
