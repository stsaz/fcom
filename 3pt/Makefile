# Third-party code with easy interfaces (wrappers)

include config.mk
export FCOM_3PT := $(dir $(firstword $(MAKEFILE_LIST)))
LIBS := aes md5 sha

default: $(LIBS)
# 	$(SUBMAKE) md5check

.PHONY: aes
aes: AES.a
AES.a: $(wildcard $(FCOM_3PT)/aes/*.patch)
	$(MAKE) -f $(FCOM_3PT)/aes/Makefile

.PHONY: md5
md5: MD5.a
MD5.a:
	$(MAKE) -f $(FCOM_3PT)/md5/Makefile

.PHONY: sha
sha: SHA256.a
SHA256.a:
	$(MAKE) -f $(FCOM_3PT)/sha/Makefile

md5write:
	cd $(FCOM_3PT) && md5sum -b \
		files \
		>packages.md5

md5check:
	cd $(FCOM_3PT) && md5sum -c packages.md5 --ignore-missing
