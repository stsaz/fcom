# Third-party code with easy interfaces (wrappers)

include config.mk
BINDIR := _$(OS)-$(CPU)
LIBS := aes md5 sha

default:
	$(MKDIR) $(BINDIR)
	$(MAKE) $(LIBS)

.PHONY: aes
aes: $(BINDIR)/AES.a
$(BINDIR)/AES.a:
	$(MAKE) -C aes -I ..
	cp -au aes/*.a $(BINDIR)

.PHONY: md5
md5: $(BINDIR)/MD5.a
$(BINDIR)/MD5.a:
	$(MAKE) -C md5 -I ..
	cp -au md5/*.a $(BINDIR)

.PHONY: sha
sha: $(BINDIR)/SHA256.a
$(BINDIR)/SHA256.a:
	$(MAKE) -C sha -I ..
	cp -au sha/*.a $(BINDIR)

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
