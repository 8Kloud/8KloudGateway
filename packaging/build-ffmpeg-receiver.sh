#!/bin/sh
set -eu

# Build the small LGPL FFmpeg link set used by Gateway. The pinned patch adds
# the AOM AV1-over-MPEG-TS draft demux path; SRT remains entirely in Gateway.
# Audio parsers/decoders let stream probing finish promptly, and the
# extract_extradata filter gives the Matroska recorder the codec headers
# it needs; without them recording fails and probing waits out its window.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prefix=${1:-$root/build/ffmpeg-lgpl}
work=$root/build/ffmpeg-src
ffmpeg_tag=${FFMPEG_TAG:-n8.0.3}
headers_tag=${NV_CODEC_HEADERS_TAG:-n13.0.19.0}
patch=$root/packaging/patches/ffmpeg/0001-av1-mpegts-draft.patch

missing=
for tool in git make cc pkg-config nasm; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
  echo "ERROR: missing build tools:$missing" >&2
  echo "  sudo apt install git build-essential nasm pkg-config" >&2
  exit 1
fi
mkdir -p "$work"

if [ ! -d "$work/nv-codec-headers" ]; then
  git clone --depth 1 --branch "$headers_tag" \
    https://github.com/FFmpeg/nv-codec-headers.git "$work/nv-codec-headers"
fi
make -C "$work/nv-codec-headers" PREFIX="$prefix" install

if [ ! -d "$work/ffmpeg" ]; then
  git clone --depth 1 --branch "$ffmpeg_tag" \
    https://git.ffmpeg.org/ffmpeg.git "$work/ffmpeg"
fi
if git -C "$work/ffmpeg" apply --reverse --check "$patch" >/dev/null 2>&1; then
  echo "AV1 MPEG-TS patch already applied"
else
  git -C "$work/ffmpeg" apply --check "$patch"
  git -C "$work/ffmpeg" apply "$patch"
fi

cd "$work/ffmpeg"
make distclean >/dev/null 2>&1 || true
PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
./configure \
  --prefix="$prefix" \
  --enable-shared \
  --disable-static \
  --disable-programs \
  --disable-doc \
  --disable-everything \
  --disable-avdevice \
  --disable-avfilter \
  --disable-network \
  --disable-autodetect \
  --enable-ffnvcodec \
  --enable-cuvid \
  --enable-nvdec \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-decoder=h264,hevc,av1,aac,aac_latm,mp2,mp3,ac3,eac3,opus \
  --enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec \
  --enable-parser=h264,hevc,av1,aac,aac_latm,ac3,mpegaudio,opus \
  --enable-bsf=extract_extradata,aac_adtstoasc \
  --enable-demuxer=mpegts \
  --enable-muxer=mpegts,matroska \
  --enable-protocol=file \
  --enable-pic

# configure only warns when a requested component loses a dependency; make
# sure nothing the gateway relies on was silently dropped.
for component in H264_NVDEC_HWACCEL HEVC_NVDEC_HWACCEL AV1_NVDEC_HWACCEL \
                 H264_DECODER HEVC_DECODER AV1_DECODER MPEGTS_DEMUXER \
                 MATROSKA_MUXER EXTRACT_EXTRADATA_BSF AAC_PARSER AAC_DECODER; do
  grep -q "#define CONFIG_$component 1" config_components.h || {
    echo "ERROR: FFmpeg configure dropped $component; see ffbuild/config.log" >&2
    exit 1
  }
done

make -j"$(nproc)"
make install

echo "built: $prefix"
echo "configure Gateway with:"
echo "  PKG_CONFIG_PATH=$prefix/lib/pkgconfig cmake --preset release -DKLOUDGATEWAY_REQUIRE_APP=ON"
