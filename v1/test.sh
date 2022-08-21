set -x
set -e

if test "$1" == "copy" ; then

	rm -rf fcomtest/* || true
	mkdir -p fcomtest
	cd fcomtest
	echo hello >file

	# file -> file
	../fcom copy "file" -o "file.out"
	diff file file.out

	# file -> file (exists)
	../fcom copy "file" -o "file.out" || true

	# file -> dir/file
	mkdir dir
	../fcom copy "file" -C "dir"
	diff file dir/file

	# file -> dir/file
	../fcom copy "file" -C "dir" -o "testfile2"
	diff file dir/testfile2

	# dir -> dir
	mkdir dirempty
	../fcom copy "dirempty" -o "dirempty2"
	test -d dirempty2

	# Verify
	../fcom copy "file" -o "file.out.verify" --verify -f

	# Crypt
	../fcom copy "file" -o "file.out.encrypt" --encrypt="123"
	../fcom copy "file.out.encrypt" -o "file.out.decrypt" --decrypt="123"
	diff file file.out.decrypt
	../fcom copy "file.out.encrypt" -o "file.out.decrypt" --decrypt="123" --verify -f
	diff file file.out.decrypt

	# Recursive: dir -> dir
	mkdir -p dirempty dircopy
	../fcom copy -R "dir" "dirempty" -C "dircopy" -v
	test -d dircopy/dir
	diff dir/file dircopy/dir/file
	test -d dircopy/dirempty
	rm -rf dircopy

	# Recursive: dir/dir -> dir/dir
	mkdir dir/d2 dircopy
	echo hello >dir/d2/file
	../fcom copy -R `realpath .`"/dir" -C "dircopy" -v
	diff dir/d2/file dircopy/dir/d2/file
	rm -rf dircopy dir

	# Recursive Exclude: dir -> dir
	mkdir dir dir/d2 dircopy
	echo hello >dir/file
	echo hello >dir/file.doc
	echo hello >dir/file.docx
	echo hello >dir/file.txt
	../fcom copy -R "dir" -C "dircopy" -E "*.txt" -E "*.doc" -v
	diff dir/file dircopy/dir/file
	diff dir/file.docx dircopy/dir/file.docx
	! test -f dircopy/dir/file.doc
	! test -f dircopy/dir/file.txt
	rm -rf dircopy/*

	# Recursive Include: dir -> dir
	../fcom copy -R "dir" -C "dircopy" -I "*.txt" -I "*.doc" -v
	diff dir/file.doc dircopy/dir/file.doc
	diff dir/file.txt dircopy/dir/file.txt
	! test -f dircopy/dir/file.docx
	! test -f dircopy/dir/file
	rm -rf dircopy/*

	# Recursive Include Exclude: dir -> dir
	../fcom copy -R "dir" -C "dircopy" -I "*.d*" -E "*.doc" -v
	diff dir/file.docx dircopy/dir/file.docx
	! test -f dircopy/dir/file.doc
	! test -f dircopy/dir/file.txt
	! test -f dircopy/dir/file
	rm -rf dircopy/*

	# # Recursive Include: --include rejects directory
	# ../fcom copy -R "dir" -C "dircopy" -I "*.d*" -v
	# ! test -d dircopy/dir/d2

	# # Recursive Include: --include accepts directory
	# echo hello >dir/d2/file.doc
	# ../fcom copy -R "dir" -C "dircopy" -I "*.d*" -v
	# diff dir/d2/file.doc dircopy/dir/d2/file.doc
fi

echo DONE
