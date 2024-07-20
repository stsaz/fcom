# Third-party code with easy interfaces (wrappers)

include config.mk
export FCOM_3PT := $(dir $(firstword $(MAKEFILE_LIST)))
LIBS := jpeg-turbo png

default: $(LIBS)
	$(SUBMAKE) md5check

.PHONY: jpeg-turbo
jpeg-turbo: libjpeg-turbo-ff.$(SO)
libjpeg-turbo-ff.$(SO):
	$(MAKE) -f $(FCOM_3PT)/jpeg-turbo/Makefile

.PHONY: png
png: libpng-ff.$(SO)
libpng-ff.$(SO):
	$(MAKE) -f $(FCOM_3PT)/zlib/Makefile
	$(MAKE) -f $(FCOM_3PT)/png/Makefile

md5write:
	cd $(FCOM_3PT) && md5sum -b \
		jpeg-turbo/2.1.5.1.tar.gz \
		png/libpng-1.6.37.tar.xz \
		zlib/zlib-1.2.11.tar.xz \
		>packages.md5

md5check:
	cd $(FCOM_3PT) && md5sum -c packages.md5 --ignore-missing
