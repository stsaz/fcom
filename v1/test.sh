# fcom/v1 tester

set -x
set -e

CMD=$1

rm -rf ./fcomtest ; mkdir fcomtest

if test "$CMD" == "copy" ; then

	cd fcomtest
	echo hello >file

	# file -> file
	../fcom copy "file" -o "file.out"
	diff file file.out

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

elif test "$CMD" == "hex" ; then

	echo abc123 >fcomtest/hex
	echo qwerqwerqwerqwerqwerqwerqwerqwer >fcomtest/hex2
	./fcom hex fcomtest/hex*

elif test "$CMD" == "list" ; then

	./fcom list "fcomtest"
	echo fcomtest >fcomtest/list

	./fcom list "@fcomtest/list"
	echo fcomtest | ./fcom list "@"

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
	test -f "fcomtest/unbranch/a/new"

	# unbranch-flat
	cd fcomtest
	echo hi >>unbranch/a/hi
	../fcom move --unbranch-flat "./unbranch" -v
	test -f "hi"
	cd ..

elif test "$CMD" == "sync" ; then

	rm -f fcomtest.snap
	echo hello >fcomtest/file

	# write snapshot
	./fcom sync "fcomtest" --snapshot -o "fcomtest.snap" -v
	cat fcomtest.snap

	# diff 2 dirs
	cd fcomtest
	mkdir -p left right left/d right/d
	echo eq >left/d/eq ; cp -a left/d/eq right/d/eq
	echo moved >left/d/mv ; cp -a left/d/mv right/mv
	echo leftmod >left/d/mod
	echo rightmod >right/d/mod
	echo l >left/l
	echo r >right/r
	../fcom sync --diff "left" -o "right" -v

	# sync 2 dirs
	../fcom sync "left" -o "right" -v --add
	../fcom sync "left" -o "right" -v --delete -f

	# write snapshot, diff snapshot and dir
	../fcom sync "left" --snapshot -o "fcomtest.snap" -v
	../fcom sync --diff --source-snap "fcomtest.snap" -o "right" -v

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

elif test "$CMD" == "zip" ; then

	mkdir fcomtest/zipdir
	echo 123 >fcomtest/zipdir/file
	./fcom zip -R "fcomtest/zipdir" -o "fcomtest/zip.zip"
	test -f "fcomtest/zip.zip"
	unzip -t "fcomtest/zip.zip"

elif test "$CMD" == "all" ; then

	sh $0 copy
	sh $0 hex
	sh $0 list
	sh $0 move
	sh $0 sync
	sh $0 textcount
	sh $0 touch
	sh $0 trash
	sh $0 zip

else
	exit 1
fi

rm -rf ./fcomtest
echo DONE
