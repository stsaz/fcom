fcom is a fast file commander for Windows, Linux and FreeBSD.

Note: devel version.

Features:
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

	fcom ungz - unpack .gz files
	fcom unxz - unpack .xz files
		Unpack to the current directory
			fcom unxz ./file1.xz ./file2.xz
		Specify output file
			fcom unxz ./file1.xz -o ./file1.txt

	fcom untar - unpack .tar files
	fcom unzip - unpack .zip files
	fcom un7z - unpack .7z files
		Unpack 2 files from gzip-compressed tar archive to the specified directory
			fcom untar ./arc.tar.gz --member=file1.txt --member=file2.txt --outdir=mydir
