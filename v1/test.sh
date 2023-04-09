#!/bin/bash
# fcom/v1 tester

CMDS_OPS=(copy hex list md5 move pic sync textcount touch trash utf8)
# listdisk mount
CMDS_PACK=(gz iso tar un7z unxz zip zst unpack)

if test "$#" -lt 1 ; then
	echo Usage: bash test.sh all
	echo Usage: bash test.sh CMD...
	echo "CMD: ${CMDS_OPS[@]} ${CMDS_PACK[@]}"
	exit 1
fi

set -x
set -e

CMDS=("$@")
if test "$1" == "all" ; then
	CMDS=("${CMDS_OPS[@]}")
	CMDS+=("${CMDS_PACK[@]}")
fi

for CMD in "${CMDS[@]}" ; do

rm -rf ./fcomtest ; mkdir fcomtest

if test "$CMD" == "copy" ; then

	cd fcomtest
	echo hello >file

	# file -> file
	chmod a+x file
	../fcom copy "file" -o "file.out"
	diff file file.out
	ls -l file file.out

	# file -> file (exists)
	../fcom copy "file" -o "file.out" || true

	# file -> file (overwrite)
	../fcom copy "file" -o "file.out" --overwrite

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

	# Update
	../fcom copy --update "file" -o "testfile2" -v

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

	# Recursive Include-Exclude: dir -> dir
	../fcom copy -R "dir" -C "dircopy" -I "*.d*" -E "*.doc" -v
	diff dir/file.docx dircopy/dir/file.docx
	! test -f dircopy/dir/file.doc
	! test -f dircopy/dir/file.txt
	! test -f dircopy/dir/file
	rm -rf dircopy/*

	# Recursive Include: --include rejects directory
	../fcom copy -R "dir" -C "dircopy" -I "*.d*" -v
	! test -d dircopy/dir/d2
	rm -rf dircopy/*

	# Recursive Include: --include accepts directory
	echo hello >dir/d2/file.doc
	../fcom copy -R "dir" -C "dircopy" -I "*.d*" -v
	diff dir/d2/file.doc dircopy/dir/d2/file.doc

	cd ..

elif test "$CMD" == "hex" ; then

	echo abc123 >fcomtest/hex
	echo qwerqwerqwerqwerqwerqwerqwerqwer >fcomtest/hex2
	./fcom hex fcomtest/hex*

elif test "$CMD" == "list" ; then

	./fcom list "fcomtest"
	echo fcomtest >fcomtest/list

	./fcom list "@fcomtest/list"
	echo fcomtest | ./fcom list "@"

elif test "$CMD" == "md5" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	echo qwerqwerqwerqwerqwerqwerqwerqwer >fcomtest/file2
	./fcom md5 "fcomtest/file" "fcomtest/file2"

elif test "$CMD" == "move" ; then

	mkdir fcomtest/unbranch
	mkdir fcomtest/unbranch/a

	# unbranch
	echo hi >>fcomtest/unbranch/a/hi
	./fcom move --unbranch "fcomtest/unbranch" -v
	test -f "fcomtest/unbranch - a - hi"

	# unbranch + replace
	echo hi >>fcomtest/unbranch/a/hi
	./fcom move --unbranch "fcomtest/unbranch" --replace="a - /new - " -v
	test -f "fcomtest/unbranch - new - hi"

	# replace
	echo hi >>fcomtest/unbranch/a/test
	./fcom move "fcomtest/unbranch/a/test" --replace="test/new" -v
	test -f fcomtest/unbranch/a/new

	# unbranch-flat
	cd fcomtest
	echo hi >>unbranch/a/hi
	../fcom move --unbranch-flat "./unbranch" -v
	test -f hi
	cd ..

elif test "$CMD" == "pic" ; then

	spectacle -o fcomtest/file.bmp -b

	./fcom pic "fcomtest/file.bmp" -o "fcomtest/file2.bmp"
	test -f fcomtest/file2.bmp
	# diff fcomtest/file.bmp fcomtest/file2.bmp

elif test "$CMD" == "sync" ; then

	echo hello >fcomtest/file

	# write snapshot
	./fcom sync "fcomtest" --snapshot -o "fcomtest/fcomtest.snap" -v
	cat fcomtest/fcomtest.snap

	# diff 2 dirs
	cd fcomtest
	mkdir -p left right left/d right/d
	echo eq >left/d/eq ; cp -a left/d/eq right/d/eq
	echo moved >left/d/mv ; cp -a left/d/mv right/mv
	echo leftmod >left/d/mod
	echo rightmod >right/d/mod
	echo l >left/l
	echo r >right/r
	../fcom sync --diff="" --source-path-strip1 --target-path-strip1 "left" -o "right" -v
	../fcom sync --diff="" --source-path-strip1 --target-path-strip1 --plain --add "left" -o "right" -v
	../fcom sync --diff="" --source-path-strip1 --target-path-strip1 --plain --update "left" -o "right" -v

	# write snapshot, diff snapshot and dir
	../fcom sync --snapshot "left" -o "fcomtest.snap" -v -f
	../fcom sync --source-path-strip1 --target-path-strip1 --diff="" --source-snap "fcomtest.snap" -o "right" -v 2>LOG
	grep 'moved:1  add:1  del:1  upd:1  eq:1  total:5/5' LOG

	# diff 2 snapshots
	../fcom sync --snapshot "right" -o "fcomtest-right.snap" -v -f
	../fcom sync --source-path-strip1 --target-path-strip1 --diff="" --source-snap "fcomtest.snap" --target-snap -o "fcomtest-right.snap" -v 2>LOG
	grep 'moved:1  add:1  del:1  upd:1  eq:1  total:5/5' LOG

	# snapshot 2 dirs
	../fcom sync --snapshot "left" "right" -o "fcomtest2.snap" -v -f
	cat fcomtest2.snap

	# sync 2 dirs
	../fcom sync --source-path-strip1 --target-path-strip1 "left" -o "right" -v --add
	../fcom sync --source-path-strip1 --target-path-strip1 "left" -o "right" -v --delete -f

	cd ..

elif test "$CMD" == "textcount" ; then

	echo 123 >>fcomtest/textcount
	echo 3456 >>fcomtest/textcount
	echo 7890 >>fcomtest/textcount
	./fcom textcount -R "fcomtest" -v

elif test "$CMD" == "touch" ; then

	echo 123 >fcomtest/touch
	echo 123 >fcomtest/touch2
	./fcom touch "fcomtest/touch" --date="2022-09-01 01:02:03" -v
	ls -l fcomtest/touch
	./fcom touch "fcomtest/touch" --date="2022-09-01" -v
	ls -l fcomtest/touch
	./fcom touch "fcomtest/touch2" --reference="fcomtest/touch" -v
	ls -l fcomtest/touch2
	./fcom touch "fcomtest/touch" -v
	ls -l fcomtest/touch

	mkdir fcomtest/dirtouch fcomtest/dirtouch/d2
	echo 123 >fcomtest/dirtouch/touch
	echo 123 >fcomtest/dirtouch/d2/touch2
	./fcom touch -R "fcomtest/dirtouch" --date="2022-09-01 01:02:03" -v
	ls -Rl fcomtest/dirtouch

elif test "$CMD" == "trash" ; then
	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom trash "fcomtest/trash" "fcomtest/trash2" -v -f

	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom trash "fcomtest/trash" "fcomtest/trash2" --rename -v -f

	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom trash "fcomtest/trash" "fcomtest/trash2" --wipe --rename -v -f

elif test "$CMD" == "utf8" ; then

	# ./fcom utf8 "fcomtest/file-utf16" -o "fcomtest/file-utf8"
	# diff fcomtest/file fcomtest/file-utf8
	echo

# PACK

elif test "$CMD" == "tar" ; then

	mkdir fcomtest/tardir fcomtest/untardir
	echo 1234567890123456789012345678901234567890 >fcomtest/tardir/file

	./fcom tar "fcomtest/tardir" -o "fcomtest/tar.tar"
	./fcom untar "fcomtest/tar.tar" -C "fcomtest/untardir" -v
	diff fcomtest/tardir/file fcomtest/untardir/fcomtest/tardir/file

	./fcom untar "fcomtest/tar.tar" -C "fcomtest/untardir" -l -v

elif test "$CMD" == "un7z" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	7z a fcomtest/7z.7z fcomtest/file
	./fcom un7z "fcomtest/7z.7z" -C "fcomtest/un7z" -v
	diff fcomtest/un7z/fcomtest/file fcomtest/file

	./fcom un7z "fcomtest/7z.7z" -C "fcomtest/un7z" -l -v

elif test "$CMD" == "zip" ; then

	mkdir fcomtest/zipdir fcomtest/unzipdir
	echo 1234567890123456789012345678901234567890 >fcomtest/zipdir/file

	./fcom zip "fcomtest/notfound" "fcomtest/zipdir" -o "fcomtest/zip.zip" || true
	# unzip -t "fcomtest/zip.zip"
	./fcom unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f -v
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" --method "zstd" -f
	./fcom unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f -v
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" --method "store" -f
	# unzip -t "fcomtest/zip.zip"
	./fcom unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f -v
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	# --members-from-file
	echo file1 >fcomtest/zipdir/file1
	echo file2 >fcomtest/zipdir/file2
	echo file3 >fcomtest/zipdir/file3
	echo fcomtest/zipdir/file1 >fcomtest/LIST
	echo fcomtest/zipdir/file3 >>fcomtest/LIST
	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" -f
	./fcom unzip "fcomtest/zip.zip" -l --members-from-file="fcomtest/LIST"

	# --autodir
	./fcom unzip "fcomtest/zip.zip" -C "fcomtest" --autodir
	diff fcomtest/zipdir/file1 fcomtest/zip/fcomtest/zipdir/file1

elif test "$CMD" == "gz" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	./fcom gz "fcomtest/file" -C "fcomtest"
	./fcom gz "fcomtest/file" -o "STDOUT" >>fcomtest/file.gz
	echo 1234567890123456789012345678901234567890 >>fcomtest/file
	./fcom ungz "fcomtest/file.gz" -o "fcomtest/file-d" -v
	diff fcomtest/file-d fcomtest/file

elif test "$CMD" == "zst" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	./fcom zst "fcomtest/file" -C "fcomtest"
	./fcom zst "fcomtest/file" -o "STDOUT" >>fcomtest/file.zst
	echo 1234567890123456789012345678901234567890 >>fcomtest/file
	./fcom unzst "fcomtest/file.zst" -o "fcomtest/file-d" -v
	diff fcomtest/file-d fcomtest/file

elif test "$CMD" == "unxz" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	xz "fcomtest/file" -c >>fcomtest/file.xz
	./fcom unxz "fcomtest/file.xz" -o "fcomtest/file-d" -v
	diff fcomtest/file-d fcomtest/file

elif test "$CMD" == "iso" ; then

	mkdir fcomtest/isodir fcomtest/unisodir
	echo 1234567890123456789012345678901234567890 >fcomtest/isodir/file

	cd fcomtest
	../fcom iso "isodir" -o "iso.iso"
	../fcom uniso "iso.iso" -C "unisodir" -v
	diff isodir/file unisodir/isodir/file

	../fcom uniso "iso.iso" -C "unisodir" -l -v
	cd ..

elif test "$CMD" == "unpack" ; then

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	./fcom gz "fcomtest/file" -o "fcomtest/gz.gz"
	./fcom tar "fcomtest/file" -o "fcomtest/tar.tar"
	./fcom zip "fcomtest/file" -o "fcomtest/zip.zip"
	./fcom zst "fcomtest/file" -o "fcomtest/zst.zst"

	./fcom unpack "fcomtest/tar.tar" "fcomtest/zip.zip" -C "fcomtest" --autodir -v
	diff fcomtest/file fcomtest/tar/fcomtest/file
	diff fcomtest/file fcomtest/zip/fcomtest/file

	./fcom unpack "fcomtest/gz.gz" -C "fcomtest/gz" -v
	diff fcomtest/file fcomtest/gz/file

	./fcom unpack "fcomtest/zst.zst" -C "fcomtest/zst" -v
	diff fcomtest/file fcomtest/zst/zst

	./fcom zst "fcomtest/tar.tar" -o "fcomtest/tarzst.tar.zst"
	./fcom unpack "fcomtest/tarzst.tar.zst" -C "fcomtest/tarzst" -v
	diff fcomtest/file fcomtest/tarzst/fcomtest/file

	./fcom gz "fcomtest/tar.tar" -o "fcomtest/targz.tar.gz"
	./fcom unpack "fcomtest/targz.tar.gz" -C "fcomtest/targz" -v
	diff fcomtest/file fcomtest/targz/fcomtest/file

	# xz "fcomtest/tar.tar" -o "fcomtest/tarxz.tar.xz"
	# ./fcom unpack "fcomtest/tarxz.tar.xz" -C "fcomtest/tarxz"
	# diff fcomtest/file fcomtest/tarxz/fcomtest/file

else
	exit 1
fi

done

rm -rf ./fcomtest
echo DONE
