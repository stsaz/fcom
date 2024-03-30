#!/bin/bash

# fcom: cross-build on Linux for Debian-buster

set -xe

if ! test -d "../fcom" ; then
	exit 1
fi

if ! podman container exists fcom_debianbuster_build ; then
	if ! podman image exists fcom-debianbuster-builder ; then
		# Create builder image
		cat <<EOF | podman build -t fcom-debianbuster-builder -f - .
FROM debian:buster-slim
RUN apt update && \
 apt install -y \
  gcc g++ make
RUN apt install -y \
 libgtk-3-dev
RUN apt install -y \
 zstd unzip p7zip \
 yasm nasm \
 cmake patch dos2unix curl
EOF
	fi

	# Create builder container
	podman create --attach --tty \
	 -v `pwd`/..:/src \
	 --name fcom_debianbuster_build \
	 fcom-debianbuster-builder \
	 bash -c 'cd /src/fcom && source ./build_linux.sh'
fi

# Prepare build script
cat >build_linux.sh <<EOF
set -xe

cd ../ffpack
make -j8
make md5check
cd ../fcom

cd 3pt
make -j8
# make md5check
make install
cd ..

cd 3pt-pic
make -j8
make md5check
cd ..

mkdir -p _linux-amd64
make -j8 \
 -C _linux-amd64 \
 -f ../Makefile \
 ROOT=../.. \
 $@
make -j8 app \
 -C _linux-amd64 \
 -f ../Makefile \
 ROOT=../..

cd _linux-amd64/fcom-1
bash ../../test.sh all
EOF

# Build inside the container
podman start --attach fcom_debianbuster_build
