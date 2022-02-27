# alib3

include makeconf
BINDIR := _$(OS)-amd64

all:
	$(MAKE) -Rr -C aes

install:
	mkdir -p $(BINDIR)
	cp -u aes/*.a \
		$(BINDIR)
clean:
	$(MAKE) -C aes clean
