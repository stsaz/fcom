#!/bin/bash
# fcom tester

TESTS=()
TESTS+=(copy list move sync touch trash)
TESTS+=(help hex md5 textcount utf8 html)
CMDS_WIN=(reg_search)
# pic unico

if test "$#" -lt 1 ; then
	echo "Usage: bash test.sh (all | CMD...)"
	echo "CMD: ${TESTS[@]}"
	exit 1
fi

CMDS=("$@")
if test "$1" == "all" ; then
	CMDS=("${TESTS[@]}")
fi

set -x
set -e

test_help() {
	./fcom -h
}

test_copy_update() {
	cd fcomtest
	echo hello >file

	../fcom -V copy --update "file" -o "file-u" ; diff file file-u
	../fcom -D copy --update "file" -o "file-u" | grep -E 'update.*skipping' ; diff file file-u

	echo hellohello >file-rd
	touch "file-rd" -t 12310000
	../fcom -V copy --update --replace-date "file" -o "file-rd" | grep 'replace date'
	../fcom list -l "file" "file-rd"

	cd ..
}

test_copy_recursive() {
	cd fcomtest
	echo hello >file
	mkdir -p dir
	echo hello >dir/file

	# dir -> dir
	mkdir -p dirempty dircopy
	../fcom -V copy "dir" "dirempty" -C "dircopy"
	diff dir/file dircopy/dir/file
	test -d dircopy/dirempty
	rm -rf dircopy

	# dir/dir -> dir/dir
	mkdir dir/d2 dircopy
	echo hello >dir/d2/file
	../fcom -V copy `realpath .`"/dir" -C "dircopy"
	diff dir/d2/file dircopy/dir/d2/file
	rm -rf dircopy dir

	# Exclude: dir -> dir
	mkdir dir dir/d2 dircopy
	echo hello >dir/file
	echo hello >dir/file.doc
	echo hello >dir/file.docx
	echo hello >dir/file.txt
	echo hello >dir/d2/file.docx
	../fcom -V -D copy "dir" -C "dircopy" -E "*/d2" -E "*.txt" -E "*.doc"
	diff dir/file dircopy/dir/file
	diff dir/file.docx dircopy/dir/file.docx
	test -f dircopy/dir/file.doc && false
	test -f dircopy/dir/file.txt && false
	test -d dircopy/dir/d2 && false
	rm -rf dircopy/*

	# Include: dir -> dir
	../fcom -V copy "dir" -C "dircopy" -I "*.txt" -I "*.doc"
	diff dir/file.doc dircopy/dir/file.doc
	diff dir/file.txt dircopy/dir/file.txt
	test -f dircopy/dir/file.docx && false
	test -f dircopy/dir/file && false
	rm -rf dircopy/*

	# Include-Exclude: dir -> dir
	../fcom -V copy "dir" -C "dircopy" -I "*.d*" -E "*.doc"
	diff dir/file.docx dircopy/dir/file.docx
	test -f dircopy/dir/file.doc && false
	test -f dircopy/dir/file.txt && false
	test -f dircopy/dir/file && false
	rm -rf dircopy/*

	# Include: --Include accepts directory
	echo hello >dir/d2/file.doc
	../fcom -V copy "dir" -C "dircopy" -I "*.doc"
	diff dir/d2/file.doc dircopy/dir/d2/file.doc

	cd ..
}

test_copy() {

	cd fcomtest
	echo hello >file

	# file -> file
	chmod a+x file
	../fcom copy "file" -o "file.out"
	diff file file.out
	ls -l file file.out

	# file -> file (exists)
	! ../fcom copy "file" -o "file.out"

	# file -> file (overwrite)
	../fcom copy "file" -o "file.out" --overwrite
	diff file file.out

	echo 123456789 >file.out
	../fcom copy "file" -o "file.out" --write-into --overwrite
	diff file file.out

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

	# symlink
	ln -s file filelink
	../fcom copy "filelink" -o "filelink2"
	test "$(readlink "filelink2")" == "file"

	# Verify
	../fcom copy "file" -o "file.out.md5" --md5
	../fcom copy "file" -o "file.out.verify" --verify -f

	# Crypt
	../fcom copy "file" -o "file.out.encrypt" --encrypt "123"
	../fcom copy "file.out.encrypt" -o "file.out.decrypt" --decrypt "123"
	diff file file.out.decrypt
	../fcom copy "file.out.encrypt" -o "file.out.decrypt" --decrypt "123" --verify -f
	diff file file.out.decrypt

	cd ..

	test_copy_update
	test_copy_recursive
}

test_hex() {

	echo abc123 >fcomtest/hex
	echo qwerqwerqwerqwerqwerqwerqwerqwer >fcomtest/hex2
	./fcom hex fcomtest/hex*
}

test_list() {
	echo hello >fcomtest/list
	./fcom list
	./fcom list "fcomtest" "."
	test "$(./fcom list --oneline "fcomtest" "./fcom")" == '"fcomtest/list" "./fcom" '
	./fcom list -l "fcomtest" "."
}

test_md5() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	echo qwerqwerqwerqwerqwerqwerqwerqwer >fcomtest/file2
	local rr='21740c1ad4d727f1a0f6159fc84c44d8 *fcomtest/file
fd25ea23edc3df0d9c8ecd50a808c014 *fcomtest/file2'

	local r=$(./fcom md5 "fcomtest/file" "fcomtest/file2")
	test "$r" == "$rr"
	./fcom md5 "fcomtest/file" "fcomtest/file2" -o "fcomtest/md5"
	test "$(cat fcomtest/md5)" == "$rr"

	echo zxcvzxcv >fcomtest/file3
	./fcom md5 -u fcomtest/md5 "fcomtest/file" "fcomtest/file2" "fcomtest/file3"

	./fcom -V md5 -c fcomtest/md5

	echo 1 >>fcomtest/file
	! ./fcom -V md5 -c fcomtest/md5
}

test_move() {

	mkdir fcomtest/unbranch
	mkdir fcomtest/unbranch/a

	# unbranch
	echo hi >>fcomtest/unbranch/a/hi
	./fcom -V move --unbranch "fcomtest/unbranch"
	test -f "fcomtest/unbranch - a - hi"

	# unbranch + replace
	echo hi >>fcomtest/unbranch/a/hi
	./fcom -V move --unbranch "fcomtest/unbranch" --search "a - " --replace "new - "
	test -f "fcomtest/unbranch - new - hi"

	# replace
	echo hi >>fcomtest/unbranch/a/test
	./fcom -V move "fcomtest/unbranch/a/test" --search "test" --replace "new"
	test -f fcomtest/unbranch/a/new

	# replace in directory name
	mkdir -p fcomtest/dir1
	./fcom -V move "fcomtest/dir1" --search "1" --replace "2"
	! test -d fcomtest/dir1
	test -d fcomtest/dir2

	# unbranch-flat
	cd fcomtest
	echo hi >>unbranch/a/hi
	../fcom -V move --unbranch-flat "./unbranch"
	test -f hi
	cd ..

	# tree
	mkdir -p fcomtest/idir fcomtest/idir/idir2
	echo hi >>fcomtest/idir/idir2/hi
	./fcom move fcomtest/idir -C fcomtest/odir --tree
	test -f fcomtest/odir/fcomtest/idir/idir2/hi
}

test_pic() {

	spectacle -o fcomtest/fileo.bmp -b

	# bmp -> bmp
	./fcom pic "fcomtest/fileo.bmp" -o "fcomtest/file.bmp"
	test -f fcomtest/file.bmp
	# diff fcomtest/fileo.bmp fcomtest/file.bmp

	# bmp -> jpg
	./fcom pic "fcomtest/file.bmp" -o "fcomtest/file.jpg"
	test -f fcomtest/file.jpg

	# bmp -> png
	./fcom pic "fcomtest/file.bmp" -o "fcomtest/file.png"
	test -f fcomtest/file.png

	# png -> jpg
	./fcom pic "fcomtest/file.png" -o "fcomtest/filepng.jpg"
	diff fcomtest/file.jpg fcomtest/filepng.jpg

	# jpg -> png
	./fcom pic "fcomtest/file.jpg" -o "fcomtest/filejpg.png"
	test -f fcomtest/filejpg.png

	# jpg -> bmp
	./fcom pic "fcomtest/file.jpg" -o "fcomtest/filejpg.bmp"
	test -f fcomtest/filejpg.bmp

	# png -> bmp
	./fcom pic "fcomtest/file.png" -o "fcomtest/filepng.bmp"
	test -f fcomtest/filepng.bmp

	# autoname
	mkdir fcomtest/multi
	cd fcomtest/multi
	../../fcom -V pic "../file.bmp" "../filepng.bmp" -o ".jpg"
	cd ../..
	diff fcomtest/multi/file.jpg fcomtest/multi/filepng.jpg

	# autoname + -C
	mkdir -p fcomtest/dir1 fcomtest/dir1/dir2
	mv fcomtest/file.bmp fcomtest/filepng.bmp fcomtest/dir1/dir2/
	./fcom -V pic fcomtest/dir1 -C "fcomtest/multi" -o ".jpg"
	diff fcomtest/multi/dir1/dir2/file.jpg fcomtest/multi/dir1/dir2/filepng.jpg
}

test_unico() {

	./fcom unico "fmedia.ico"
	./fcom -V pic "fmedia-1.png" -o ".bmp"
}

test_reg_search() {

	wine ./fcom reg search "HKEY_CURRENT_USER" "CaretWidth" "MenuShowDelay" >LOG
	grep 'HKEY_CURRENT_USER\\Control Panel\\Desktop\\CaretWidth = "0x00000001 (1)"' LOG
	grep 'HKEY_CURRENT_USER\\Control Panel\\Desktop\\MenuShowDelay = "400"' LOG
}

test_sync_prepare() {
	cd fcomtest

	mkdir -p left right left/d right/d

	echo eq >left/d/eq ; cp -a left/d/eq right/d/eq
	echo moved >left/d/mv ; cp -a left/d/mv right/mv
	echo renamed >left/d/renamed ; cp -a left/d/renamed right/renamed2

	echo leftmod >left/d/mod
	sleep .1
	echo rightmod >right/d/mod

	echo mod_date >left/d/mod_date
	echo mod_date >right/d/mod_date ; ../fcom touch --date "2001-01-01 01:01:01" "right/d/mod_date"

	echo l >left/l
	echo r >right/r

	cd ..
}

test_sync() {

	echo hello >fcomtest/file

	# write snapshot
	./fcom -V sync "fcomtest" --snapshot -o "fcomtest/fcomtest.snap"
	cat fcomtest/fcomtest.snap

	test_sync_prepare
	cd fcomtest

	# diff 2 dirs
	../fcom -V sync --diff U --diff-time-sec "left" -o "right"
	../fcom -V sync --diff "" "left" -o "right"
	../fcom -V sync --diff "" --diff-no-dir "left" -o "right"
	../fcom -V sync --diff "A" --plain "left" -o "right"
	../fcom -V sync --diff "U" --plain "left" -o "right"

	echo left >left/d/l_old
	touch left/d/l_old -t 0001010000
	../fcom -V sync --diff "" --recent 30 "left" -o "right"
	rm left/d/l_old

	# write snapshot, diff snapshot and dir
	../fcom -V sync --snapshot "left" -o "fcomtest.snap" -f
	../fcom -V sync --diff "" --source-snap "fcomtest.snap" -o "right" >LOG
	grep 'moved:1  add:2  del:2  upd:3  eq:1  total:7/7' LOG

	# diff 2 snapshots
	../fcom -V sync --snapshot "right" -o "fcomtest-right.snap" -f
	../fcom -V sync --diff "" --source-snap "fcomtest.snap" --target-snap -o "fcomtest-right.snap" >LOG
	grep 'moved:1  add:2  del:2  upd:3  eq:1  total:7/7' LOG

	# snapshot 2 dirs
	../fcom -V sync --snapshot "left" "right" -o "fcomtest2.snap" -f
	cat fcomtest2.snap

	# sync 2 dirs
	../fcom -V sync "left" -o "right" --add
	../fcom -V sync "left" -o "right" --delete -f

	cd ..
}

test_sync_zip() {
	mkdir fcomtest/dir fcomtest/dir2
	./fcom touch "fcomtest/dir/file1"
	./fcom touch "fcomtest/dir/file2"
	./fcom touch "fcomtest/dir2/file3"
	./fcom zip "fcomtest/dir" "fcomtest/dir2" -o "fcomtest/sync.zip"
	./fcom -D sync --snapshot "fcomtest" --zip-expand -o "fcomtest/snap.txt"
	cat fcomtest/snap.txt
}

test_gsync() {
	test_sync_prepare
	cd fcomtest
	mkdir -p left-a left-b
	touch left-a/a
	touch left-b/b
	../fcom gsync "./left*" "./right"
}

test_textcount() {

	echo 123 >>fcomtest/textcount
	echo 3456 >>fcomtest/textcount
	echo 7890 >>fcomtest/textcount
	./fcom -V textcount -R "fcomtest"
}

test_touch() {

	echo 123 >fcomtest/touch
	echo 123 >fcomtest/touch2
	./fcom -V touch "fcomtest/touch" --date "2022-09-01 01:02:03"
	ls -l fcomtest/touch
	./fcom -V touch "fcomtest/touch" --date "2022-09-01"
	ls -l fcomtest/touch
	./fcom -V touch "fcomtest/touch2" --reference "fcomtest/touch"
	ls -l fcomtest/touch2
	./fcom -V touch "fcomtest/touch"
	ls -l fcomtest/touch

	mkdir fcomtest/dirtouch fcomtest/dirtouch/d2
	echo 123 >fcomtest/dirtouch/touch
	echo 123 >fcomtest/dirtouch/d2/touch2
	./fcom -V touch -R "fcomtest/dirtouch" --date "2022-09-01 01:02:03"
	ls -Rl fcomtest/dirtouch
}

test_trash() {
	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom -V trash "fcomtest/trash" "fcomtest/trash2" -f

	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom -V trash "fcomtest/trash" "fcomtest/trash2" --rename -f

	echo 123 >fcomtest/trash
	echo 123 >fcomtest/trash2
	./fcom -V trash "fcomtest/trash" "fcomtest/trash2" --wipe --rename -f
}

test_utf8() {

	# ./fcom utf8 "fcomtest/file-utf16" -o "fcomtest/file-utf8"
	# diff fcomtest/file fcomtest/file-utf8
	echo
}

test_html() {
	cat <<EOF >fcomtest/html
<tag attr="123"></tag>
<tag attr="234"/>
EOF
	./fcom html "fcomtest/html" --filter "tag.attr" | grep 123
	./fcom html "fcomtest/html" --filter "tag.attr" | grep 234
}

source "$(dirname $0)/test-pack.sh"

mkdir -p fcomtest

for cmd in "${CMDS[@]}" ; do

	rm -rf ./fcomtest/*
	test_$cmd

done

rm -rf ./fcomtest
echo DONE
