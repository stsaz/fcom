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

* Cross-Build on Linux for Debian-bookworm:

	```sh
	bash xbuild.sh
	```

* Cross-Build on Linux for Windows/AMD64:

	```sh
	OS=windows \
		bash xbuild.sh
	```

## Step 2 (Option 2). Native Build

```sh
make -j8 -C ffpack

cd fcom
mkdir -p 3pt/_linux-amd64
make -j8 \
 -C 3pt/_linux-amd64 \
 -f ../Makefile \
 -I ..

mkdir -p 3pt-pic/_linux-amd64
make -j8 \
 -C 3pt-pic/_linux-amd64 \
 -f ../Makefile \
 -I ..

make -j8
```

`fcom-1` is the app directory.
