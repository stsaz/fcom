# Third-party code with easy interfaces (wrappers)

include config.mk
BINDIR := _$(OS)-$(CPU)

all: aes md5 sha

.PHONY: aes
aes: $(BINDIR)/AES.a
$(BINDIR)/AES.a:
	$(MAKE) -C aes -I ..

.PHONY: md5
md5: $(BINDIR)/MD5.a
$(BINDIR)/MD5.a:
	$(MAKE) -C md5 -I ..

.PHONY: sha
sha: $(BINDIR)/SHA256.a
$(BINDIR)/SHA256.a:
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
