# Third-party code with easy interfaces (wrappers)

include config.mk
TGT_DIR := _$(OS)-$(CPU)

default:
	mkdir -p $(TGT_DIR)
	$(MAKE) build

build: jpeg-turbo png

.PHONY: jpeg-turbo
$(TGT_DIR)/libjpeg-turbo-ff.$(SO):
	$(MAKE) -C jpeg-turbo -I ..
	cp jpeg-turbo/*.$(SO) $(TGT_DIR)
jpeg-turbo: $(TGT_DIR)/libjpeg-turbo-ff.$(SO)

.PHONY: png
$(TGT_DIR)/libpng-ff.$(SO):
	$(MAKE) -C zlib -I ..
	$(MAKE) -C png -I ..
	cp png/*.$(SO) $(TGT_DIR)
png: $(TGT_DIR)/libpng-ff.$(SO)

clean:
	$(MAKE) -C jpeg-turbo clean
	$(MAKE) -C png clean
	$(MAKE) -C zlib clean

md5write:
	md5sum -b \
		jpeg-turbo/2.1.5.1.tar.gz \
		png/libpng-1.6.37.tar.xz \
		zlib/zlib-1.2.11.tar.xz \
		>packages.md5

md5check:
	md5sum -c packages.md5 --ignore-missing
