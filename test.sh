#!/bin/bash
# fcom/v1 tester

CMDS_FS=(copy list move sync touch trash)
CMDS_OPS=(help hex md5 textcount utf8 html disana)
CMDS_WIN=(reg-search)
CMDS_PACK=(gz iso tar un7z unxz zip zst unpack)
# pic listdisk mount unico

if test "$#" -lt 1 ; then
	echo Usage: bash test.sh all
	echo Usage: bash test.sh CMD...
	echo "CMD: ${CMDS_FS[@]} ${CMDS_OPS[@]} ${CMDS_PACK[@]}"
	exit 1
fi

CMDS=("$@")
if test "$1" == "all" ; then
	CMDS=("${CMDS_FS[@]}")
	CMDS+=("${CMDS_OPS[@]}")
	CMDS+=("${CMDS_PACK[@]}")
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

test_reg-search() {

	wine ./fcom reg search "HKEY_CURRENT_USER" "CaretWidth" "MenuShowDelay" >LOG
	grep 'HKEY_CURRENT_USER\\Control Panel\\Desktop\\CaretWidth = "0x00000001 (1)"' LOG
	grep 'HKEY_CURRENT_USER\\Control Panel\\Desktop\\MenuShowDelay = "400"' LOG
}

test_sync_prepare() {
	cd fcomtest

	mkdir -p left right left/d right/d

	echo eq >left/d/eq ; cp -a left/d/eq right/d/eq
	echo moved >left/d/mv ; cp -a left/d/mv right/mv

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
	grep 'moved:1  add:1  del:1  upd:3  eq:1  total:6/6' LOG

	# diff 2 snapshots
	../fcom -V sync --snapshot "right" -o "fcomtest-right.snap" -f
	../fcom -V sync --diff "" --source-snap "fcomtest.snap" --target-snap -o "fcomtest-right.snap" >LOG
	grep 'moved:1  add:1  del:1  upd:3  eq:1  total:6/6' LOG

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
	../fcom gsync left right
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

test_disana() {
	objdump -d ./fcom | ./fcom disana
}

# PACK

test_tar() {

	mkdir fcomtest/tardir fcomtest/untardir
	echo 1234567890123456789012345678901234567890 >fcomtest/tardir/file

	./fcom tar "fcomtest/tardir" -o "fcomtest/tar.tar"
	./fcom -V untar "fcomtest/tar.tar" -C "fcomtest/untardir"
	diff fcomtest/tardir/file fcomtest/untardir/fcomtest/tardir/file

	./fcom -V untar "fcomtest/tar.tar" -C "fcomtest/untardir" -l
}

test_un7z() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	if test -f /usr/bin/7zr ; then
		7zr a fcomtest/7z.7z fcomtest/file
	else
		7z a fcomtest/7z.7z fcomtest/file
	fi
	./fcom -V un7z "fcomtest/7z.7z" -C "fcomtest/un7z"
	diff fcomtest/un7z/fcomtest/file fcomtest/file

	./fcom un7z "fcomtest/7z.7z" -C "fcomtest/un7z" -l
	./fcom -V un7z "fcomtest/7z.7z" -C "fcomtest/un7z" -l
}

test_zip() {

	mkdir fcomtest/zipdir fcomtest/unzipdir
	echo 1234567890123456789012345678901234567890 >fcomtest/zipdir/file

	cd fcomtest ; ../fcom zip "zipdir"; cd .. ; test -f fcomtest/zipdir.zip ; rm fcomtest/zipdir.zip
	./fcom zip "fcomtest/zipdir" -C "fcomtest" ; test -f fcomtest/zipdir.zip
	./fcom zip "fcomtest/zipdir" -C "fcomtest" -o "z.zip" ; test -f fcomtest/z.zip

	./fcom zip "fcomtest/notfound" "fcomtest/zipdir" -o "fcomtest/zip.zip" || true
	# unzip -t "fcomtest/zip.zip"
	./fcom -V unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" --method "zstd" -f
	./fcom -V unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" --method "store" -f
	# unzip -t "fcomtest/zip.zip"
	./fcom -V unzip "fcomtest/zip.zip" -C "fcomtest/unzipdir" -f
	diff fcomtest/zipdir/file fcomtest/unzipdir/fcomtest/zipdir/file

	# --members-from-file
	echo file1 >fcomtest/zipdir/file1
	echo file2 >fcomtest/zipdir/file2
	echo file3 >fcomtest/zipdir/file3

	echo fcomtest/zipdir/file1 >fcomtest/LIST
	echo '*/file3' >>fcomtest/LIST
	./fcom zip "fcomtest/zipdir" -o "fcomtest/zip.zip" -f
	local list=$(./fcom unzip "fcomtest/zip.zip" -l --members-from-file "fcomtest/LIST")
	grep file1 <<< $list
	grep file2 <<< $list && false
	grep file3 <<< $list

	local list=$(./fcom unzip "fcomtest/zip.zip" -l -m "fcomtest/zipdir/file1" -m "fcomtest/zipdir/file3")
	grep file1 <<< $list
	grep file2 <<< $list && false
	grep file3 <<< $list

	# --autodir
	./fcom unzip "fcomtest/zip.zip" -C "fcomtest" --autodir
	diff fcomtest/zipdir/file1 fcomtest/zip/fcomtest/zipdir/file1

	# --each
	echo file1 >fcomtest/a
	echo file2 >fcomtest/b
	echo file3 >fcomtest/c
	./fcom zip --each "fcomtest/a" "fcomtest/b" "fcomtest/c" -C "fcomtest"
	./fcom unzip -l "fcomtest/a.zip" "fcomtest/b.zip" "fcomtest/c.zip"
}

test_gz() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	echo 1234567890123456789012345678901234567890 >fcomtest/file2

	./fcom -V gz "fcomtest/file" "fcomtest/file2" -C "fcomtest"
	test -f fcomtest/file.gz
	test -f fcomtest/file2.gz

	./fcom gz "fcomtest/file" -o "STDOUT" >>fcomtest/file.gz
	echo 1234567890123456789012345678901234567890 >>fcomtest/file
	./fcom -V ungz "fcomtest/file.gz" -o "fcomtest/file-d"
	diff fcomtest/file-d fcomtest/file
}

test_zst() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	echo 1234567890123456789012345678901234567890 >fcomtest/file2

	./fcom -V zst "fcomtest/file" "fcomtest/file2" -C "fcomtest"
	test -f fcomtest/file.zst
	test -f fcomtest/file2.zst

	./fcom zst "fcomtest/file" -o "STDOUT" >>fcomtest/file.zst
	echo 1234567890123456789012345678901234567890 >>fcomtest/file
	./fcom -V unzst "fcomtest/file.zst" -o "fcomtest/file-d"
	diff fcomtest/file-d fcomtest/file
}

test_unxz() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file
	xz "fcomtest/file" -c >>fcomtest/file.xz
	./fcom -V unxz "fcomtest/file.xz" -o "fcomtest/file-d"
	diff fcomtest/file-d fcomtest/file
}

test_iso() {

	mkdir fcomtest/isodir fcomtest/unisodir
	echo 1234567890123456789012345678901234567890 >fcomtest/isodir/file

	cd fcomtest
	../fcom iso "isodir" -o "iso.iso"
	../fcom -V uniso "iso.iso" -C "unisodir"
	diff isodir/file unisodir/isodir/file

	../fcom -V uniso "iso.iso" -C "unisodir" -l
	cd ..
}

test_unpack() {

	echo 1234567890123456789012345678901234567890 >fcomtest/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/tar.tar"
	./fcom -V pack "fcomtest/file" -o "fcomtest/zip.zip"
	./fcom -V unpack "fcomtest/tar.tar" "fcomtest/zip.zip" -C "fcomtest" --autodir
	diff fcomtest/file fcomtest/tar/fcomtest/file
	diff fcomtest/file fcomtest/zip/fcomtest/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/zipx.zipx"
	./fcom -V unpack "fcomtest/zipx.zipx" -C "fcomtest" --autodir
	diff fcomtest/file fcomtest/zipx/fcomtest/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/gz.gz"
	./fcom -V unpack "fcomtest/gz.gz" -C "fcomtest/gz"
	diff fcomtest/file fcomtest/gz/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/zst.zst"
	./fcom -V unpack "fcomtest/zst.zst" -C "fcomtest/zst"
	diff fcomtest/file fcomtest/zst/zst

	./fcom -V pack "fcomtest/file" -o "fcomtest/tarzst.tar.zst"
	./fcom -V unpack "fcomtest/tarzst.tar.zst" -C "fcomtest/tarzst"
	diff fcomtest/file fcomtest/tarzst/fcomtest/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/tartargz.tar.gz"
	./fcom -V unpack "fcomtest/tartargz.tar.gz" -C "fcomtest/tartargz"
	diff fcomtest/file fcomtest/tartargz/fcomtest/file

	./fcom -V pack "fcomtest/file" -o "fcomtest/targz.tgz"
	./fcom -V unpack "fcomtest/targz.tgz" -C "fcomtest/targz"
	diff fcomtest/file fcomtest/targz/fcomtest/file

	./fcom unpack "fcomtest/tartargz.tar.gz" -l | grep fcomtest/file

	# xz "fcomtest/tar.tar" -o "fcomtest/tarxz.tar.xz"
	# ./fcom unpack "fcomtest/tarxz.tar.xz" -C "fcomtest/tarxz"
	# diff fcomtest/file fcomtest/tarxz/fcomtest/file
}

mkdir -p fcomtest

for CMD in "${CMDS[@]}" ; do

	rm -rf ./fcomtest/*
	test_$CMD

done

rm -rf ./fcomtest
echo DONE
