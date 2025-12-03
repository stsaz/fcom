#!/bin/bash

# fcom: cross-build on Linux for Linux/AMD64 | Windows/AMD64

IMAGE_NAME=fcom-debianbw-builder
CONTAINER_NAME=fcom_debianBW_build
BUILD_TARGET=linux
if test "$OS" == "windows" ; then
	IMAGE_NAME=fcom-win64-builder
	CONTAINER_NAME=fcom_win64_build
	BUILD_TARGET=win64
fi
ARGS=${@@Q}

set -xe

if ! test -d "../fcom" ; then
	exit 1
fi

image_linux() {
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
 gcc g++
RUN apt install -y \
 libgtk-3-dev
EOF
}

image_windows() {
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
}

if ! podman container exists $CONTAINER_NAME ; then
	if ! podman image exists $IMAGE_NAME ; then
		# Create builder image
		image_$OS
	fi

	# Create builder container
	podman create --attach --tty \
	 -v `pwd`/..:/src \
	 --workdir /src/fcom \
	 --name $CONTAINER_NAME \
	 $IMAGE_NAME \
	 bash ./build_$BUILD_TARGET.sh
fi

if ! podman container top $CONTAINER_NAME ; then
	cat >build_$BUILD_TARGET.sh <<EOF
sleep 600
EOF
	# Start container in background
	podman start --attach $CONTAINER_NAME &
	sleep .5
	while ! podman container top $CONTAINER_NAME ; do
		sleep .5
	done
fi

ODIR=_linux-amd64
ARGS_ARCH=
TESTS_RUN=all
MAKE_JOBS=8
if test "$OS" == "windows" ; then
	ODIR=_windows-amd64
	ARGS_ARCH="OS=windows \
COMPILER=gcc \
CROSS_PREFIX=x86_64-w64-mingw32-"
	TESTS_RUN=
fi

# Prepare build script
cat >build_$BUILD_TARGET.sh <<EOF
set -xe

mkdir -p ../ffpack/$ODIR
make -j$MAKE_JOBS \
 -C ../ffpack/$ODIR \
 -f ../Makefile \
 -I .. \
 $ARGS_ARCH

mkdir -p 3pt/$ODIR
make -j$MAKE_JOBS \
 -C 3pt/$ODIR \
 -f ../Makefile \
 -I .. \
 $ARGS_ARCH

mkdir -p 3pt-pic/$ODIR
make -j$MAKE_JOBS \
 -C 3pt-pic/$ODIR \
 -f ../Makefile \
 -I .. \
 $ARGS_ARCH

mkdir -p $ODIR
make -j$MAKE_JOBS \
 -C $ODIR \
 -f ../Makefile \
 ROOT=../.. \
 $ARGS_ARCH \
 $ARGS

if test "$TESTS_RUN" != "" ; then
	cd $ODIR/fcom-1
	bash ../../test.sh $TESTS_RUN
fi
EOF

# Build inside the container
podman exec $CONTAINER_NAME \
 bash ./build_$BUILD_TARGET.sh
