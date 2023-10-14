## Welcome to fcom project!

fcom is a small, fast and portable file commander for Linux and Windows.
Its goal is to include the most useful functions for working with files of different types: text, binary, archives, pictures, etc.

Contents:

* [Features](#features)
* [Install on Linux](#install-on-linux)
* [Install on Windows](#install-on-windows)
* [Build](#build)
* [Third-party code](#third-party-code)

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


## Install on Linux

1. Unpack the archive to the directory of your choice, e.g. to Home/bin:

		tar xf fcom-v1.0beta1-linux-amd64.tar.zst --zstd -C ~/bin

2. Optionally, create a symbolic link:

		ln -s ~/bin/fcom-1/fcom ~/bin/fcom


## Install on Windows

1. Unpack the archive to the directory of your choice.
2. Optionally, add the path to `fcom.exe` to `PATH`.


## Build

[Building fcom](BUILDING.md)


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


## License

fcom is in the public-domain.
Third-party code is the property of their authors.
