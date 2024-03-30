# fcom: exe

%.o: $(FCOM_DIR)/src/exe/%.c
	$(C) $(CFLAGS) $< -o $@

_:=
ifeq "$(OS)" "windows"
	EXE_COFF := exe.coff
endif
exe.coff: $(FCOM_DIR)/res/exe.rc \
		$(FCOM_DIR)/res/exe.manifest
	$(WINDRES) $< $@

$(BIN): main.o args.o \
		$(EXE_COFF) \
		core.$(SO)
	$(LINK) $+ $(LINKFLAGS) $(LINK_RPATH_ORIGIN) -o $@
