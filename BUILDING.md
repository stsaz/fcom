# Building fcom

1. Create a directory for the source code:

		mkdir fcom-src && cd fcom-src

2. Download the repositories:

	```sh
	git clone --depth=1 https://github.com/stsaz/ffbase
	git clone --depth=1 https://github.com/stsaz/ffsys
	git clone --depth=1 https://github.com/stsaz/ffpack
	git clone --depth=1 https://github.com/stsaz/avpack
	git clone --depth=1 https://github.com/stsaz/fcom
	```

3. Build third-party code:

	```sh
	cd fcom/3pt
	make -j8
	make md5check
	cd ../..

	cd fcom/3pt-pic
	make -j8
	make install
	cd ../..

	cd ffpack
	make -j8
	make md5check
	cd ..
	```

4. Build fcom:

		cd fcom
		make -j8

`fcom-1` is the app directory.  Now you may move it anywhere you want (see section "Install on Linux").


## Cross-Build on Linux for Debian-buster:

	```sh
	bash xbuild-debianbuster.sh
	```

## Cross-Build on Linux for Windows/AMD64:

	```sh
	bash xbuild-win64.sh
	```
