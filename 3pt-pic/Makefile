# Third-party code with easy interfaces (wrappers)

include makeconf
BINDIR := _$(OS)-$(CPU)

all:
	$(MAKE) -Rr -C jpeg-turbo
	$(MAKE) -Rr -C zlib
	$(MAKE) -Rr -C png

install:
	mkdir -p $(BINDIR)
	cp -au \
		jpeg-turbo/*.so jpeg-turbo/*.dll \
		png/*.so png/*.dll \
		$(BINDIR)

clean:
	$(MAKE) -C jpeg-turbo clean
	$(MAKE) -C png clean
	$(MAKE) -C zlib clean

	md5sum -b \
		jpeg-turbo/2.1.5.1.tar.gz \
		png/libpng-1.6.37.tar.xz \
		zlib/zlib-1.2.11.tar.xz

md5check:
	md5sum -c packages.md5 --ignore-missing
