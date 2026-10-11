#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 INPUT_VIDEO OUTPUT_MP4 [FPS]" >&2
    exit 2
fi

input=$1
output=$2
fps=${3:-15}
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

command -v ffmpeg >/dev/null
command -v ffprobe >/dev/null
command -v magick >/dev/null 2>&1 || command -v convert >/dev/null

if command -v magick >/dev/null 2>&1; then
    imagemagick=(magick)
else
    imagemagick=(convert)
fi

# First form a letterboxed 1920x1080 image, then remove four rows from the
# top and bottom. 1920x1072 satisfies the ESP32-P4 YUV420 16x16 MCU rule.
ffmpeg -v error -i "$input" \
    -vf "scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black,crop=1920:1072:0:4,fps=${fps}" \
    -vsync 0 "$workdir/frame_%06d.png"

export workdir
export IM_CONVERT="${imagemagick[*]}"
find "$workdir" -name 'frame_*.png' -print0 | xargs -0 -P 8 -n 1 bash -c '
    png="$1"
    jpg="${png%.png}.jpg"
    $IM_CONVERT "$png" -sampling-factor 2x2 -quality 82 "$jpg"
' bash

# Copy the JPEG bitstreams into MP4 unchanged. ImageMagick emits the standard
# SOF sampling factors Y=0x22, Cb=0x11, Cr=0x11 required by this hardware path.
ffmpeg -v error -framerate "$fps" -i "$workdir/frame_%06d.jpg" \
    -c:v copy -an -movflags +faststart -y "$output"

ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,pix_fmt,width,height,r_frame_rate,duration,nb_frames \
    -of default=nw=1 "$output"
sha256sum "$output"
