# fcom tester

set -x # print commands before executing
set -e # exit if a child process reports an error

FCOM=./fcom
TESTDIR=./fcom-test
param=$1

if test "$param" = "pack" ; then
	rm -rf $TESTDIR
	$FCOM gz ./README.txt -o $TESTDIR/fcom-gz.gz
	cat $TESTDIR/fcom-gz.gz $TESTDIR/fcom-gz.gz > $TESTDIR/fcom-gz-double.gz
	$FCOM tar ./README.txt ./CHANGES.txt -o $TESTDIR/fcom-tar.tar
	$FCOM tar ./README.txt ./CHANGES.txt -o $TESTDIR/fcom-tar.tar.gz
	$FCOM zip ./README.txt ./CHANGES.txt -o $TESTDIR/fcom-zip.zip
	$FCOM iso ./README.txt ./CHANGES.txt -o $TESTDIR/fcom-iso.iso
fi

if test "$param" = "unpack" ; then
	rm -f $TESTDIR/*.txt
	$FCOM ungz $TESTDIR/fcom-gz.gz -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt

	rm -f $TESTDIR/*.txt
	$FCOM ungz $TESTDIR/fcom-gz-double.gz -C $TESTDIR
	cat ./README.txt ./README.txt > $TESTDIR/README.double.txt
	diff $TESTDIR/README.double.txt $TESTDIR/README.txt

	# rm -f $TESTDIR/*.txt
	# $FCOM unxz ./fcom-xz.xz -C $TESTDIR
	# diff ./README.txt $TESTDIR/README.txt

	rm -f $TESTDIR/*.txt
	$FCOM untar $TESTDIR/fcom-tar.tar -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM untar $TESTDIR/fcom-tar.tar.gz -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM unzip $TESTDIR/fcom-zip.zip -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM uniso $TESTDIR/fcom-iso.iso -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/fcom-gz.gz -C $TESTDIR
	# rm -f $TESTDIR/*.txt
	# $FCOM unpack ./fcom-xz.xz -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/fcom-tar.tar -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/fcom-tar.tar.gz -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/fcom-zip.zip -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/fcom-iso.iso -C $TESTDIR
fi

if test "$param" = "crypt" ; then
	$FCOM encrypt --password=123456 ./fcom -o $TESTDIR/fcom-encrypt -f
	$FCOM decrypt --password=123456 $TESTDIR/fcom-encrypt -o $TESTDIR/fcom-decrypt -f
	diff ./fcom $TESTDIR/fcom-decrypt
fi

if test "$param" = "all" ; then
	sh $0 pack
	sh $0 unpack
	sh $0 crypt
fi
