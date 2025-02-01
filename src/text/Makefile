# fcom: text

MODS += \
	disana.$(SO) \
	html.$(SO) \
	textcount.$(SO) \
	utf8.$(SO)

html.o: $(FCOM_DIR)/src/text/html.c
	$(C) $(CFLAGS) -msse4.2 $< -o $@

%.o: $(FCOM_DIR)/src/text/%.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

%.o: $(FCOM_DIR)/src/text/%.c
	$(C) $(CFLAGS) $< -o $@

disana.$(SO): disana.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@
