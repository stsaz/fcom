# fcom: file system modules

MODS += \
	copy.$(SO) \
	gsync.$(SO) \
	list.$(SO) \
	move.$(SO) \
	sync.$(SO) \
	touch.$(SO) \
	trash.$(SO)

%.o: $(FCOM_DIR)/src/fs/%.c
	$(C) $(CFLAGS) $< -o $@

%.o: $(FCOM_DIR)/src/fs/%.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

trash.$(SO): trash.o
	$(LINK) -shared $+ $(LINKFLAGS) $(LINK_DL) -o $@

list.$(SO): list.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@

copy.$(SO): copy.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@

sync.$(SO): sync.o
	$(LINKXX) -shared $+ $(LINKXXFLAGS) -o $@

FFGUI := $(ROOT)/ffgui
ifeq "$(OS)" "windows"
	CFLAGS_GUI := -Wno-missing-field-initializers
	LINKFLAGS_GUI := -lshell32 -luxtheme -lcomctl32 -lcomdlg32 -lgdi32 -lole32 -luuid
	FFGUI_OBJ := \
		ffgui-winapi.o \
		ffgui-winapi-loader.o
else
	CFLAGS_GUI := -Wno-free-nonheap-object -Wno-deprecated-declarations \
		`pkg-config --cflags gtk+-3.0`
	LINKFLAGS_GUI := `pkg-config --libs gtk+-3.0` \
		$(LINK_PTHREAD) -lm
	FFGUI_OBJ := \
		ffgui-gtk.o \
		ffgui-gtk-loader.o
endif
ifeq "$(DEBUG_GUI)" "1"
	CFLAGS_GUI += -DFFGUI_DEBUG
endif
CFLAGS_GUI += -I$(FFGUI)
CXXFLAGS_GUI := $(CXXFLAGS) $(CFLAGS_GUI)
CFLAGS_GUI := $(CFLAGS) $(CFLAGS_GUI)

%.o: $(FFGUI)/ffgui/gtk/%.c
	$(C) $(CFLAGS_GUI) $< -o $@
%.o: $(FFGUI)/ffgui/winapi/%.c
	$(C) $(CFLAGS_GUI) $< -o $@

gsync.o: $(FCOM_DIR)/src/fs/gsync.cpp
	$(CXX) $(CXXFLAGS_GUI) $< -o $@

gsync.$(SO): gsync.o \
		$(FFGUI_OBJ)
	$(LINKXX) -shared $+ $(LINKXXFLAGS) $(LINKFLAGS_GUI) -o $@

app-gsync:
	cp -au $(FCOM_DIR)/src/fs/gsync.ui fcom-1/ops
