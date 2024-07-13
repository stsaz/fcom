# Building fcom

## Step 1. Download code

```sh
mkdir fcom-src
cd fcom-src
git clone --depth=1 https://github.com/stsaz/ffbase
git clone --depth=1 https://github.com/stsaz/ffsys
git clone --depth=1 https://github.com/stsaz/ffpack
git clone --depth=1 https://github.com/stsaz/avpack
git clone --depth=1 https://github.com/stsaz/fcom
```

## Step 2. Cross-Build

* Cross-Build on Linux for Debian-buster:

	```sh
	bash xbuild-debianbuster.sh
	```

* Cross-Build on Linux for Windows/AMD64:

	```sh
	bash xbuild-win64.sh
	```

## Step 2 (Option 2). Native Build

```sh
make -j8 -C ffpack
make md5check -C ffpack

cd fcom
make -j8 -C 3pt
# make md5check

make -j8 -C 3pt-pic
make md5check -C 3pt-pic

make -j8
```

`fcom-1` is the app directory.
