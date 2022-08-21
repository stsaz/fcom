# Third-party code with easy interfaces (wrappers)

include makeconf
BINDIR := _$(OS)-amd64

all:
	$(MAKE) -Rr -C aes
	$(MAKE) -Rr -C md5
	$(MAKE) -Rr -C sha

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

md5:
	md5sum -b \
		aes/master.zip \
		>packages.md5

md5check:
	md5sum -c packages.md5 --ignore-missing
