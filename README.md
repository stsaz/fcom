fcom is a fast file commander for Windows, Linux and FreeBSD.
Its goal is to include functions for working with files of different types: text, binary, archives, pictures, etc.

Note: beta version.  Testing and feedback are welcome!

### Contents:
* FEATURES
* INSTALL
	* INSTALL ON WINDOWS
	* INSTALL ON LINUX
	* BUILD ON LINUX


---------------
FEATURES
---------------

* Files
	* fcom touch
	* fcom crc
	* fcom rename
	* fcom sync
	* fcom sync-snapshot
	* fcom peinfo
* Text files
	* fcom textcount
	* fcom utf8
* Archives
	* fcom gz
	* fcom ungz
	* fcom unxz
	* fcom tar
	* fcom zip
	* fcom iso
	* fcom untar
	* fcom unzip
	* fcom un7z
	* fcom uniso
	* fcom unpack
* Pictures
	* fcom pic-convert
	* fcom screenshots
* Windows-only
	* fcom wregfind
	* fcom gsync

### Files

#### fcom touch - create/set attributes on files or directories

Create an empty file:

	fcom touch ./myfile

Set the current date on an existing file:

	fcom touch ./myfile

Set the specified date on a new/existing file:

	fcom touch ./myfile --date="2011-01-01 00:00:00"
	fcom touch ./myfile2 --date-as=./myfile

#### fcom crc - compute file checksums

Compute CRC32 checksum:

	fcom crc myfile

#### fcom rename - rename files (search and replace)

Rename all files within the current directory - replace 'old' with 'new':

	fcom rename * --replace='old/new'

#### fcom sync - show the difference of 2 file trees

	fcom sync DIR -o DIR

#### fcom sync-snapshot - save directory tree to a file

Scan the current directory and save the snapshot to a file:

	fcom sync-snapshot -R . -o /tmp/snapshot.txt

#### fcom peinfo - Show PE format information

Show PE header, data directories, sections info:

	fcom peinfo file.exe


### Text files

#### fcom textcount - show information about text files

Count lines in all files in directory:

	fcom textcount ./mydir -R


#### fcom utf8 - convert text files to UTF-8

Convert files to UTF-8, create new files in "new/" directory (file tree is preserved):

	fcom utf8 -R text/ -o new/.txt


### Archives

#### fcom gz - pack file to .gz

Pack to separate .gz files in the current directory:

	fcom gz /file1.txt /file2.txt

#### fcom ungz - unpack .gz files
#### fcom unxz - unpack .xz files

Unpack to the current directory:

	fcom unxz ./file1.xz ./file2.xz

Specify output file:

	fcom unxz ./file1.xz -o ./file1.txt

#### fcom tar - pack files to .tar
#### fcom zip - pack files to .zip

	fcom zip /file1.txt /file2.txt -o arc.zip

Pack with `--exclude` option:

	fcom zip . -R -o arc.zip --exclude="*/.git;./tmpdir;*.bak"

Here we exclude all `.git` directories (but not files with `.git` extension), `./tmpdir` directory and all `*.bak` files.

#### fcom iso - create .iso image

	fcom iso ./mydir -o myimage.iso

#### fcom untar - unpack .tar files
#### fcom unzip - unpack .zip files
#### fcom un7z - unpack .7z files
#### fcom uniso - unpack .iso files

Unpack 2 files from gzip-compressed tar archive to the specified directory:

	fcom untar ./arc.tar.gz --member=file1.txt --member=file2.txt --outdir=mydir

#### fcom unpack - unpack archives (determine format by file extension)

	fcom unpack ./arc.tar.gz ./arc.zip ./arc.7z

### Pictures

#### fcom pic-convert - convert pictures

Convert BMP to JPEG:

	fcom pic-convert pic.bmp -o pic.jpg

Convert files to JPEG, create new files along the original files:

	fcom pic-convert -R pictures/ -o .jpg

Convert files to JPEG, create new files in "new/" directory (file tree is preserved):

	fcom pic-convert -R pictures/ -o new/.jpg

#### fcom screenshots - show GUI to save screenshots to disk (Windows)

Save screenshots of your desktop or a top-level window by pressing a global hotkey.  Supports BMP, PNG, JPEG output formats.


### Windows-only

#### fcom wregfind - search within Windows system registry

Search "sometext" in HKEY_CURRENT_USER:

	fcom wregfind sometext --member=HKCU

#### fcom gsync - show GUI to synchronize files (Windows)

How to use:
1. Select directory for "Source path" and "Target path"
2. Command -> Compare
3. Select checkboxes near the files you wish to synchronize
4. Command -> Synchronize


---------------
INSTALL
---------------

### INSTALL ON WINDOWS

1. Unpack archive to the directory of your choice, e.g. to `"C:\Program Files\fcom"`

### INSTALL ON LINUX

1. Unpack archive to the directory of your choice, e.g. to `/usr/local/fcom-0`:

		tar Jxf ./fcom-0.5-linux-amd64.tar.xz -C /usr/local

2. Optionally, create a symbolic link:

		ln -s /usr/local/fcom-0/fcom /usr/local/bin/fcom

### BUILD ON LINUX

1. Create a directory for all needed sources:

		mkdir fcom-src && cd fcom-src

2. Download all needed source repositories:

		git clone https://github.com/stsaz/ffos
		git clone https://github.com/stsaz/ff
		git clone https://github.com/stsaz/ff-3pt
		git clone https://github.com/stsaz/fcom

3. Build ff-3pt package (3rd-party libraries) or download pre-built binaries.  See `ff-3pt/README.txt` for details.

4. Build fcom:

		cd fcom
		export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../ff-3pt-bin/linux-amd64
		make install

	You can explicitly specify path to each of FF source repositories, e.g.:

		make install FFOS=~/ffos FF=~/ff FF3PT=~/ff-3pt

	Default architecture is amd64.  You can specify different target architecture like this:

		make install ARCH=i686

	You'll also need to specify the proper path to ff-3pt binaries in `LD_LIBRARY_PATH`.

5. Ready!  You can copy the directory `./fcom-0` anywhere you want (see section "INSTALL ON LINUX").


---------------
LICENSE
---------------
The code provided here is free for use in open-source and proprietary projects.
You may distribute, redistribute, modify the whole code or the parts of it, just keep the original copyright statement inside the files.


---------------
HOMEPAGE
---------------
http://github.com/stsaz/fcom
