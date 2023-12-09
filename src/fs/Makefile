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

%.o: $(FCOM_DIR)/src/fs/%.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

list.$(SO): list.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@

copy.$(SO): copy.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@
