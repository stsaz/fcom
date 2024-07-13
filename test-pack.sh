TESTS+=(gz iso tar un7z unxz zip zst unpack)

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
