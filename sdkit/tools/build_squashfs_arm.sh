#!/bin/bash
# Builds static ARMv5 (ARM926EJ-S) unsquashfs + mksquashfs (squashfs-tools 4.6.1, GPL-2.0)
# with gzip + xz support, for the on-camera firmware build. Needs zig 0.14.1 on PATH.
set -e
cd "$(dirname "$0")"; W=$PWD/work; P=$W/prefix; mkdir -p $W $P; cd $W
curl -sL -o zlib.tgz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
curl -sL -o xz.tgz https://github.com/tukaani-project/xz/releases/download/v5.6.3/xz-5.6.3.tar.gz
curl -sL -o sq.tgz https://github.com/plougher/squashfs-tools/archive/refs/tags/4.6.1.tar.gz
for f in *.tgz; do tar xzf $f; done
zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -mfloat-abi=soft -fno-sanitize=all -O2 -c ../armv5_sync.c -o armv5_sync.o
sed "s#/home/leos/lscfw/sqtools/armv5_sync.o#$W/armv5_sync.o#" ../zcc > zcc; chmod +x zcc
printf '#!/bin/sh\nexec zig ar "$@"\n' > zar; chmod +x zar
export CC=$W/zcc AR=$W/zar CFLAGS=-O2
(cd zlib-1.3.1 && CHOST=arm ./configure --static --prefix=$P && make libz.a && make install)
(cd xz-5.6.3 && ./configure --host=arm-linux-musleabi --prefix=$P --enable-static --disable-shared --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc --disable-nls --disable-threads && make && make install)
cd squashfs-tools-4.6.1/squashfs-tools
# the camera has only ~45 MB RAM: lower the hard 64 MB minimum (the build itself runs with -mem 16M)
sed -i "s/#define SQUASHFS_LOWMEM 64/#define SQUASHFS_LOWMEM 16/" mksquashfs.h
make CC=$W/zcc GZIP_SUPPORT=1 XZ_SUPPORT=1 LZO_SUPPORT=0 LZ4_SUPPORT=0 ZSTD_SUPPORT=0 LZMA_XZ_SUPPORT=0 XATTR_SUPPORT=0 EXTRA_CFLAGS="-O2 -I$P/include" EXTRA_LDFLAGS="-static -L$P/lib" mksquashfs unsquashfs
for b in mksquashfs unsquashfs; do zig objcopy --strip-all $b ../../../$b; done
echo "built: $W/mksquashfs $W/unsquashfs"
