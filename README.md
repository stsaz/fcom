# fcom

fcom is a small, fast and portable file commander for Linux and Windows.
Its goal is to include the most useful functions for working with files of different types: text, binary, archives, pictures, etc.

Contents:

* [Features](#features)
* [Install on Linux](#install-on-linux)
* [Install on Windows](#install-on-windows)
* [Build](#build)
* [Third-party code](#third-party-code)

## Features

| Operation | Description |
| --- | --- |
| **File System:** | |
| [copy](src/fs/copy.cpp)               | Copy files from one place to another, plus encryption & verification |
| [gsync](src/fs/gsync.cpp)             | Show GUI for synchronizing directories |
| [list](src/fs/list.cpp)               | List directory contents |
| [move](src/fs/move.c)                 | Move and/or rename files |
| [sync](src/fs/sync.cpp)               | Compare/synchronize directories or create a file tree snapshot |
| [touch](src/fs/touch.c)               | Change file date/time |
| [trash](src/fs/trash.c)               | Move files to user's trash directory, plus obfuscation |
| **Compress:** | |
| [gz](src/pack/gz.c)                   | Compress file into .gz |
| [zst](src/pack/zst.c)                 | Compress file into .zst |
| **Pack files:** | |
| [pack](src/pack/pack.c)               | Pack files into any supported archive type |
| [iso](src/pack/iso.c)                 | Pack files into .iso |
| [tar](src/pack/tar.c)                 | Pack files into .tar |
| [zip](src/pack/zip.c)                 | Pack files into .zip |
| **Decompress:** | |
| [ungz](src/pack/ungz.c)               | Decompress file from .gz |
| [unxz](src/pack/unxz.c)               | Decompress file from .xz |
| [unzst](src/pack/unzst.c)             | Decompress file from .zst |
| **Unpack files:** | |
| [unpack](src/pack/unpack.c)           | Unpack files from all supported archive types |
| [un7z](src/pack/un7z.c)               | Unpack files from .7z |
| [uniso](src/pack/uniso.c)             | Unpack files from .iso |
| [untar](src/pack/untar.c)             | Unpack files from .tar |
| [unzip](src/pack/unzip.c)             | Unpack files from .zip |
| **Text:** | |
| [html](src/text/html.c)               | Parse HTML data |
| [textcount](src/text/textcount.c)     | Analyze text files (e.g. print number of lines) |
| [utf8](src/text/utf8.c)               | Convert files to UTF-8 |
| **Other:** | |
| [disana](src/text/disana.c)           | Analyze disassembler listing |
| [hex](src/ops/hex.c)                  | Print file contents in hexadecimal format |
| [ico-extract](src/pic/ico-extract.c)  | Extract images from .ico |
| [md5](src/ops/md5.c)                  | Compute MD5 hash |
| [pic](src/pic/pic.c)                  | Convert images (.bmp/.jpg/.png) |
| **Windows-only:** | |
| [listdisk](src/windows/listdisk.c)    | List logical volumes |
| [mount](src/windows/mount.c)          | Mount logical volumes |
| [reg](src/windows/reg.c)              | Windows Registry utils: search |


## Install on Linux

1. Unpack the archive to the directory of your choice, e.g. to Home/bin:

	```sh
	mkdir -p ~/bin
	tar xf fcom-v1.0-beta1-linux-amd64.tar.zst -C ~/bin
	```

2. Optionally, create a symbolic link:

	```sh
	ln -s ~/bin/fcom-1/fcom ~/bin/fcom
	```


## Install on Windows

1. Unpack the archive to the directory of your choice.
2. Optionally, add the path to `fcom.exe` to `PATH`.


## Build

[Building fcom](BUILDING.md)


## Third-party code

fcom uses third-party code that implements complex algorithms such as data encryption and cryptographic hashing functions:

* [AES (Brian Gladman)](https://github.com/BrianGladman/aes)
* CRC32 (xz)
* [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)
* libpng
* lzma (xz)
* MD5 (nginx)
* SHA-256 (glibc)
* zlib
* [zstd](https://github.com/facebook/zstd)

Many thanks to all the people who created and implemented those algorithms!!!


## License

fcom is in the public-domain.
Third-party code is the property of their authors.
