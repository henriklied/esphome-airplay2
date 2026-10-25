#!/bin/sh
# Pink-noise ADTS clips -> components/fdk_bench/clips.h (gitignored, ~3.6 MB).
# aac_at is Apple's AudioToolbox encoder (macOS ffmpeg builds only).
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d)
noise() { n=$1; r=$2; i=1; in=""; while [ $i -le $n ]; do in="$in -f lavfi -i anoisesrc=c=pink:r=$r:d=3:a=0.3:s=$i"; i=$((i+1)); done; echo "$in"; }
ffmpeg -y -loglevel error $(noise 2 44100) -filter_complex "amerge=inputs=2,aformat=channel_layouts=stereo" -c:a aac_at -b:a 256k -f adts "$tmp/stereo_at.aac"
ffmpeg -y -loglevel error $(noise 6 48000) -filter_complex "amerge=inputs=6,aformat=channel_layouts=5.1" -c:a aac_at -b:a 640k -f adts "$tmp/s51_at.aac"
ffmpeg -y -loglevel error $(noise 6 48000) -filter_complex "amerge=inputs=6,aformat=channel_layouts=5.1" -c:a aac -b:a 640k -f adts "$tmp/s51_ff.aac"
for f in stereo_at s51_at s51_ff; do
  (cd "$tmp" && xxd -i -n "clip_$f" "$f.aac") | sed 's/^unsigned/static const unsigned/'
done > components/fdk_bench/clips.h
rm -r "$tmp"
