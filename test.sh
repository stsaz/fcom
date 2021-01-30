# fcom tester

set -x # print commands before executing
set -e # exit if a child process reports an error

FCOM=./fcom
TESTDIR=./fcom-test
CMD=$1

if test "$CMD" = "pack" ; then
	rm -rf $TESTDIR
	$FCOM gz ./README.txt -o $TESTDIR/test.gz
	cat $TESTDIR/test.gz $TESTDIR/test.gz > $TESTDIR/test-double.gz
	xz -c ./README.txt > $TESTDIR/README.txt.xz
	mkdir -p emptydir
	$FCOM tar emptydir ./README.txt ./CHANGES.txt -o $TESTDIR/test.tar
	$FCOM tar emptydir ./README.txt ./CHANGES.txt -o $TESTDIR/test.tar.gz
	$FCOM zip emptydir ./README.txt ./CHANGES.txt -o $TESTDIR/test.zip
	$FCOM iso emptydir ./README.txt ./CHANGES.txt -o $TESTDIR/test.iso
	7z a $TESTDIR/test.7z emptydir ./README.txt ./CHANGES.txt
fi

if test "$CMD" = "unpack" ; then
	rm -f $TESTDIR/*.txt
	$FCOM ungz $TESTDIR/test.gz -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt

	rm -f $TESTDIR/*.txt
	$FCOM ungz $TESTDIR/test-double.gz -C $TESTDIR
	cat ./README.txt ./README.txt > $TESTDIR/README.double.txt
	diff $TESTDIR/README.double.txt $TESTDIR/README.txt

	rm -f $TESTDIR/*.txt
	$FCOM unxz $TESTDIR/README.txt.xz -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt

	rm -f $TESTDIR/*.txt
	$FCOM untar $TESTDIR/test.tar -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM untar $TESTDIR/test.tar.gz -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM unzip $TESTDIR/test.zip -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM uniso $TESTDIR/test.iso -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt

	rm -f $TESTDIR/*.txt
	$FCOM un7z $TESTDIR/test.7z -C $TESTDIR
	diff ./README.txt $TESTDIR/README.txt
	diff ./CHANGES.txt $TESTDIR/CHANGES.txt
	ls $TESTDIR/emptydir

	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/test.gz -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/README.txt.xz -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/test.tar -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/test.tar.gz -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/test.zip -C $TESTDIR
	rm -f $TESTDIR/*.txt
	$FCOM unpack $TESTDIR/test.iso -C $TESTDIR
fi

if test "$CMD" = "crypt" ; then
	$FCOM encrypt --password=123456 ./fcom -o $TESTDIR/fcom-encrypt -f
	$FCOM decrypt --password=123456 $TESTDIR/fcom-encrypt -o $TESTDIR/fcom-decrypt -f
	diff ./fcom $TESTDIR/fcom-decrypt
fi

if test "$CMD" = "all" ; then
	sh $0 pack
	sh $0 unpack
	sh $0 crypt
fi

echo DONE
