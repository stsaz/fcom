#!/bin/bash

# fcom: cross-build on Linux for Windows/AMD64

set -xe

if ! test -d "../fcom" ; then
	exit 1
fi

if ! podman container exists fcom_win64_build ; then
	if ! podman image exists fcom-win64-builder ; then
		# Create builder image
		cat <<EOF | podman build -t fcom-win64-builder -f - .
FROM debian:bookworm-slim
RUN apt update && \
 apt install -y \
  make
RUN apt install -y \
  gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
RUN apt install -y \
 yasm nasm \
 zstd zip unzip bzip2 xz-utils \
 cmake patch dos2unix curl
EOF
	fi

	# Create builder container
	podman create --attach --tty \
	 -v `pwd`/..:/src \
	 --name fcom_win64_build \
	 fcom-win64-builder \
	 bash -c 'cd /src/fcom && source ./build_mingw64.sh'
fi

# Prepare build script
cat >build_mingw64.sh <<EOF
set -xe

cd ../ffpack
make -j8 \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-
make md5check
cd ../fcom

cd 3pt
make -j8 \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-
# make md5check
make install \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-
cd ..

cd 3pt-pic
make -j8 \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-
make md5check
cd ..

mkdir -p _windows-amd64
make -j8 \
 -C _windows-amd64 \
 -f ../Makefile \
 ROOT=../.. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32- \
 $@
make -j8 app \
 -C _windows-amd64 \
 -f ../Makefile \
 ROOT=../.. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-
EOF

# Build inside the container
podman start --attach fcom_win64_build
