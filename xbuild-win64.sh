#!/bin/bash

# fcom: cross-build on Linux for Windows/AMD64

IMAGE_NAME=fcom-win64-builder
CONTAINER_NAME=fcom_win64_build
ARGS=${@@Q}

set -xe

if ! test -d "../fcom" ; then
	exit 1
fi

if ! podman container exists $CONTAINER_NAME ; then
	if ! podman image exists $IMAGE_NAME ; then

		# Create builder image
		cat <<EOF | podman build -t $IMAGE_NAME -f - .
FROM debian:bookworm-slim
RUN apt update && \
 apt install -y \
  make
RUN apt install -y \
 zstd zip unzip p7zip bzip2 xz-utils \
 yasm nasm \
 cmake patch dos2unix curl
RUN apt install -y \
  gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
EOF
	fi

	# Create builder container
	podman create --attach --tty \
	 -v `pwd`/..:/src \
	 --name $CONTAINER_NAME \
	 $IMAGE_NAME \
	 bash -c 'cd /src/fcom && source ./build_win64.sh'
fi

# Prepare build script
cat >build_win64.sh <<EOF
set -xe

mkdir -p ../ffpack/_windows-amd64
make -j8 \
 -C ../ffpack/_windows-amd64 \
 -f ../Makefile \
 -I .. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-

mkdir -p 3pt/_windows-amd64
make -j8 \
 -C 3pt/_windows-amd64 \
 -f ../Makefile \
 -I .. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-

mkdir -p 3pt-pic/_windows-amd64
make -j8 \
 -C 3pt-pic/_windows-amd64 \
 -f ../Makefile \
 -I .. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32-

mkdir -p _windows-amd64
make -j8 \
 -C _windows-amd64 \
 -f ../Makefile \
 ROOT=../.. \
 OS=windows \
 COMPILER=gcc \
 CROSS_PREFIX=x86_64-w64-mingw32- \
 $ARGS
EOF

# Build inside the container
podman start --attach $CONTAINER_NAME
