fcom is a fast file commander for Windows, Linux and FreeBSD.

Note: beta version.

---------------
FEATURES
---------------
	fcom touch - create/set attributes on files or directories
		Create an empty file:
			fcom touch ./myfile
		Set the current date on an existing file:
			fcom touch ./myfile
		Set the specified date on a new/existing file:
			fcom touch ./myfile --date="2011-01-01 00:00:00"
			fcom touch ./myfile2 --date-as=./myfile

	fcom textcount - show information about text files
		Count lines in all files in directory:
			fcom textcount ./mydir -R

	fcom crc - compute file checksums
		Compute CRC32 checksum:
			fcom crc myfile

	fcom gz - pack file to .gz
		Pack to separate .gz files in the current directory
			fcom gz /file1.txt /file2.txt

	fcom ungz - unpack .gz files
	fcom unxz - unpack .xz files
		Unpack to the current directory
			fcom unxz ./file1.xz ./file2.xz
		Specify output file
			fcom unxz ./file1.xz -o ./file1.txt

	fcom tar - pack files to .tar
	fcom zip - pack files to .zip
		fcom zip /file1.txt /file2.txt -o arc.zip

	fcom untar - unpack .tar files
	fcom unzip - unpack .zip files
	fcom un7z - unpack .7z files
	fcom uniso - unpack .iso files
		Unpack 2 files from gzip-compressed tar archive to the specified directory
			fcom untar ./arc.tar.gz --member=file1.txt --member=file2.txt --outdir=mydir

	fcom unpack - unpack archives (determine format by file extension)
			fcom unpack ./arc.tar.gz ./arc.zip ./arc.7z

	fcom wregfind - search within Windows system registry
		Search "sometext" in HKEY_CURRENT_USER
			fcom wregfind sometext --member=HKCU

	fcom screenshots - show GUI to save screenshots to disk (Windows)

	fcom pic-convert - convert pictures
		Convert BMP to JPEG
			fcom pic-convert pic.bmp -o pic.jpg
		Convert files to JPEG, create new files along the original files
			fcom pic-convert -R pictures/ -o .jpg
		Convert files to JPEG, create new files in "new/" directory (file tree is preserved)
			fcom pic-convert -R pictures/ -o new/.jpg


---------------
LICENSE
---------------
The code provided here is free for use in open-source and proprietary projects.
You may distribute, redistribute, modify the whole code or the parts of it, just keep the original copyright statement inside the files.


---------------
HOMEPAGE
---------------
http://github.com/stsaz/fcom
