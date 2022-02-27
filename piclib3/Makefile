# piclib3

include makeconf
BINDIR := _$(OS)-amd64

all:
	$(MAKE) -Rr -C jpeg-turbo
	$(MAKE) -Rr -C zlib
	$(MAKE) -Rr -C png

install:
	mkdir -p $(BINDIR)
	cp -u jpeg-turbo/*.$(SO) \
		png/*.$(SO) \
		$(BINDIR)

clean:
	$(MAKE) -Rr -C jpeg-turbo clean
	$(MAKE) -Rr -C zlib clean
	$(MAKE) -Rr -C png clean
