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
