# Third-party code with easy interfaces (wrappers)

include config.mk
BINDIR := _$(OS)-$(CPU)

all:
	$(MAKE) -C aes -I ..
	$(MAKE) -C md5 -I ..
	$(MAKE) -C sha -I ..

install:
	mkdir -p $(BINDIR)
	cp -au \
		aes/*.a \
		md5/*.a \
		sha/*.a \
		$(BINDIR)

clean:
	$(MAKE) -C aes clean
	$(MAKE) -C md5 clean
	$(MAKE) -C sha clean

md5write:
	md5sum -b \
		files \
		>packages.md5

md5check:
	md5sum -c packages.md5 --ignore-missing
