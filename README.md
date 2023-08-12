## Welcome to fcom project!

fcom is a small, fast and portable file commander for Linux and Windows.
Its goal is to include the most useful functions for working with files of different types: text, binary, archives, pictures, etc.

Contents:

* [Features](#features)
* [Third-party code](#third-party-code)
* [Install on Linux](#install-on-linux)
* [Install on Windows](#install-on-windows)
* [Build on Linux](#build-on-linux)

## Features

```
  General:
    copy                Copy files from one place to another, plus encryption & verification
    list                List directory contents
    move                Move and/or rename files
    touch               Change file date/time
    trash               Move files to user's trash directory, plus obfuscation

  Compress files:
    gz                  Compress file into .gz
    zst                 Compress file into .zst

  Pack files:
    iso                 Pack files into .iso
    tar                 Pack files into .tar
    zip                 Pack files into .zip

  Decompress files:
    ungz                Decompress file from .gz
    unxz                Decompress file from .xz
    unzst               Decompress file from .zst

  Unpack files:
    unpack              Unpack files from all supported archive types
    un7z                Unpack files from .7z
    uniso               Unpack files from .iso
    untar               Unpack files from .tar
    unzip               Unpack files from .zip

  Other:
    hex                 Print file contents in hexadecimal format
    ico-extract         Extract images from .ico
    listdisk            List logical volumes (Windows)
    md5                 Compute MD5 hash
    mount               Mount logical volumes (Windows)
    pic                 Convert images (.bmp/.jpg/.png)
    reg                 Windows Registry utils: search
    sync                Compare/synchronize directories or create a file tree snapshot
    textcount           Analyze text files (e.g. print number of lines)
    utf8                Convert files to UTF-8
```

## Third-party code

fcom uses third-party code that implements complex algorithms such as data encryption and cryptographic hashing functions:

* AES (Brian Gladman)
* CRC32 (xz)
* libjpeg-turbo
* libpng
* lzma (xz)
* MD5 (nginx)
* SHA-256 (glibc)
* zlib
* zstd

Many thanks to all the people who created and implemented those algorithms!!!


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
		git clone --depth=1 https://github.com/stsaz/ffpack
		git clone --depth=1 https://github.com/stsaz/avpack
		git clone --depth=1 -b v1 https://github.com/stsaz/fcom

3. Build third-party code:

		cd fcom/3pt
		make -j8
		make md5check
		make install
		cd ../..

		cd fcom/3pt-pic
		make -j8
		make md5check
		make install
		cd ../..

		cd ffpack
		make -j8
		make md5check
		make install
		cd ..

4. Build fcom:

		cd fcom
		make -j8

`fcom-1` is the app directory.  Now you may move it anywhere you want (see section "Install on Linux").


## License

fcom is in the public-domain.
Third-party code is the property of their authors.
