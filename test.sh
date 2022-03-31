# fcom tester

# gz iso tar tar-gz tar-zstd zip-store zip-deflate zip-zstd crypt

set -x # print commands before executing
set -e # exit if a child process reports an error

for CMD in $@
do
	rm -rf ./test

	if test "$CMD" = "7z" ; then
		mkdir -p ./test emptydir
		7z a ./test/test.7z emptydir/ ./README.txt ./CHANGES.txt

		./fcom un7z ./test/test.7z -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt
		ls ./test/emptydir
		rm ./test/*.txt

		./fcom un7z ./test/test.7z -C ./test --member='R*.txt'
		test -f ./test/README.txt
		test ! -f ./test/CHANGES.txt
		rm ./test/*.txt

		./fcom un7z ./test/test.7z -C ./test --member='README.txt;CHANGES.txt'
		test -f ./test/README.txt
		test -f ./test/CHANGES.txt

	elif test "$CMD" = "gz" ; then
		./fcom gz ./README.txt -o ./test/test.gz
		cat ./test/test.gz ./test/test.gz > ./test/test-double.gz

		./fcom ungz ./test/test.gz -C ./test
		diff ./README.txt ./test/README.txt

		rm -f ./test/*.txt
		./fcom ungz ./test/test-double.gz -C ./test
		cat ./README.txt ./README.txt > ./test/README.double.txt
		diff ./test/README.double.txt ./test/README.txt

	elif test "$CMD" = "iso" ; then

		./fcom iso mod/ ./README.txt ./CHANGES.txt -o ./test/test.iso

		./fcom uniso ./test/test.iso -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "tar" ; then

		./fcom tar mod/ ./README.txt ./CHANGES.txt -o ./test/test.tar

		./fcom untar ./test/test.tar -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "tar-gz" ; then

		./fcom tar mod/ ./README.txt ./CHANGES.txt -o ./test/test.tar.gz

		./fcom untar ./test/test.tar.gz -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "tar-zstd" ; then

		./fcom tar mod/ ./README.txt ./CHANGES.txt -o ./test/test.tar.zst

		./fcom untar ./test/test.tar.zst -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "zip-store" ; then

		./fcom zip --compression-method=store mod/ ./README.txt ./CHANGES.txt -o ./test/test.zip

		./fcom unzip ./test/test.zip -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "zip-deflate" ; then

		./fcom zip --compression-method=deflate mod/ ./README.txt ./CHANGES.txt -o ./test/test.zip

		./fcom unzip ./test/test.zip -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "zip-zstd" ; then

		./fcom zip --compression-method=zstd mod/ ./README.txt ./CHANGES.txt -o ./test/test.zip

		./fcom unzip ./test/test.zip -C ./test
		diff ./README.txt ./test/README.txt
		diff ./CHANGES.txt ./test/CHANGES.txt

	elif test "$CMD" = "xz" ; then
		mkdir -p ./test
		xz -c ./README.txt > ./test/README.txt.xz

		./fcom unxz ./test/README.txt.xz -C ./test
		diff ./README.txt ./test/README.txt

	elif test "$CMD" = "crypt" ; then
		./fcom encrypt --password=123456 ./fcom -o ./test/fcom-encrypt
		./fcom decrypt --password=123456 ./test/fcom-encrypt -o ./test/fcom-decrypt
		diff ./fcom ./test/fcom-decrypt

	else
		echo "Invalid command: $CMD"

	fi

done

rm -rf ./test
echo DONE
