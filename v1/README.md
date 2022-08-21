## Welcome to fcom project!

fcom is a small, fast and portable file commander for Linux and Windows.
Its goal is to include the most useful functions for working with files of different types: text, binary, archives, pictures, etc.

Contents:

* Features
* Third-party code
* Install on Linux
* Install on Windows
* Build on Linux

## Features

* `copy` - Copy files from one place to another, plus encryption & verification


## Third-party code

fcom uses third-party code that implements complex algorithms such as data encryption and cryptographic hashing functions: AES (Brian Gladman), MD5 (nginx), SHA-512 (glibc).


## Install on Linux

1. Unpack the archive to the directory of your choice, e.g. to Home/bin:

		tar xf fcom-v1.0beta1-linux-amd64.tar.zst --zstd -C ~/bin

2. Optionally, create a symbolic link:

		ln -s ~/bin/fcom-1/fcom ~/bin/fcom


## Install on Windows

1. Unpack the archive to the directory of your choice.
2. Optionally, add the path to `fcom.exe` to `PATH`.


## Build on Linux

1. Create a directory for the source code:

		mkdir fcom-src && cd fcom-src

2. Download the repositories:

		git clone --depth=1 https://github.com/stsaz/ffbase
		git clone --depth=1 https://github.com/stsaz/ffos
		git clone --depth=1 https://github.com/stsaz/fcom

3. Build third-party code:

		cd fcom/3pt
		make -Rr -j8
		make md5check
		make install
		cd ../..

4. Build fcom:

		cd fcom
		make -Rr -j8

`fcom-1` is the installation directory.  Now you may move it anywhere you want (see section "Install on Linux").


## License

fcom's code is absolutely free.
