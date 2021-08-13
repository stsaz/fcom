# fcom tester

# gz iso tar tar-gz tar-zstd zip-store zip-deflate zip-zstd crypt

set -x # print commands before executing
set -e # exit if a child process reports an error

FCOM=./fcom
TESTDIR=./fcom-test
RM='rm -rf'

for CMD in $@
do
	$RM $TESTDIR

	if test "$CMD" = "7z" ; then
		mkdir -p $TESTDIR emptydir
		7z a $TESTDIR/test.7z emptydir/ ./README.txt ./CHANGES.txt

		$FCOM un7z $TESTDIR/test.7z -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt
		ls $TESTDIR/emptydir
		$RM emptydir

	elif test "$CMD" = "gz" ; then
		$FCOM gz ./README.txt -o $TESTDIR/test.gz
		cat $TESTDIR/test.gz $TESTDIR/test.gz > $TESTDIR/test-double.gz

		$FCOM ungz $TESTDIR/test.gz -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt

		rm -f $TESTDIR/*.txt
		$FCOM ungz $TESTDIR/test-double.gz -C $TESTDIR
		cat ./README.txt ./README.txt > $TESTDIR/README.double.txt
		diff $TESTDIR/README.double.txt $TESTDIR/README.txt

	elif test "$CMD" = "iso" ; then

		$FCOM iso mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.iso

		$FCOM uniso $TESTDIR/test.iso -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "tar" ; then

		$FCOM tar mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.tar

		$FCOM untar $TESTDIR/test.tar -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "tar-gz" ; then

		$FCOM tar mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.tar.gz

		$FCOM untar $TESTDIR/test.tar.gz -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "tar-zstd" ; then

		$FCOM tar mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.tar.zst

		$FCOM untar $TESTDIR/test.tar.zst -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "zip-store" ; then

		$FCOM zip --compression-method=store mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.zip

		$FCOM unzip $TESTDIR/test.zip -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "zip-deflate" ; then

		$FCOM zip --compression-method=deflate mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.zip

		$FCOM unzip $TESTDIR/test.zip -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "zip-zstd" ; then

		$FCOM zip --compression-method=zstd mod/ ./README.txt ./CHANGES.txt -o $TESTDIR/test.zip

		$FCOM unzip $TESTDIR/test.zip -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt
		diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	elif test "$CMD" = "xz" ; then
		mkdir -p $TESTDIR
		xz -c ./README.txt > $TESTDIR/README.txt.xz

		$FCOM unxz $TESTDIR/README.txt.xz -C $TESTDIR
		diff ./README.txt $TESTDIR/README.txt

	elif test "$CMD" = "crypt" ; then
		$FCOM encrypt --password=123456 ./fcom -o $TESTDIR/fcom-encrypt
		$FCOM decrypt --password=123456 $TESTDIR/fcom-encrypt -o $TESTDIR/fcom-decrypt
		diff ./fcom $TESTDIR/fcom-decrypt

	fi

done

$RM $TESTDIR
echo DONE
