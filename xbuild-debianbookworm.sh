#!/bin/bash

# fcom: cross-build on Linux for Debian-bookworm

IMAGE_NAME=fcom-debianbookworm-builder
CONTAINER_NAME=fcom_debianbookworm_build
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
 gcc g++
RUN apt install -y \
 libgtk-3-dev
EOF
	fi

	# Create builder container
	podman create --attach --tty \
	 -v `pwd`/..:/src \
	 --name $CONTAINER_NAME \
	 $IMAGE_NAME \
	 bash -c 'cd /src/fcom && source ./build_linux.sh'
fi

# Prepare build script
cat >build_linux.sh <<EOF
set -xe

make -j8 \
 -C ../ffpack
make md5check \
 -C ../ffpack

make -j8 \
 -C 3pt
# make md5check

make -j8 \
 -C 3pt-pic
make md5check \
 -C 3pt-pic

mkdir -p _linux-amd64
make -j8 \
 -C _linux-amd64 \
 -f ../Makefile \
 ROOT=../.. \
 $ARGS

cd _linux-amd64/fcom-1
bash ../../test.sh all
EOF

# Build inside the container
podman start --attach $CONTAINER_NAME
