#!/bin/sh
# Stereo pink-noise ADTS clips -> components/aac_bench/clips.h (gitignored).
# aac_at is Apple's AudioToolbox encoder (macOS ffmpeg builds only), the
# encoder behind what Apple senders stream.
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d)
stereo() { r=$1; enc=$2; out=$3
  ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=c=pink:r=$r:d=3:a=0.3:s=1" -f lavfi -i "anoisesrc=c=pink:r=$r:d=3:a=0.3:s=2" \
    -filter_complex "amerge=inputs=2,aformat=channel_layouts=stereo" -c:a "$enc" -b:a 256k -f adts "$tmp/$out.aac"
}
stereo 44100 aac_at s441_at
stereo 48000 aac_at s48_at
stereo 44100 aac s441_ff
for f in s441_at s48_at s441_ff; do
  (cd "$tmp" && xxd -i -n "clip_$f" "$f.aac") | sed 's/^unsigned/static const unsigned/'
done > components/aac_bench/clips.h
rm -r "$tmp"
