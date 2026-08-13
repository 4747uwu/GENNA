#!/usr/bin/env bash
# Build the benchmark corpora that the tests take as argv[1].
source "$(dirname "$0")/env.sh"
C=$GENNA/corpora
mkdir -p "$C/vid"

echo "== WikiText-2 (realbench, rsynccmp) =="
if [ -f "$C/wikitext-2-raw/wiki.train.raw" ]; then
  ls -la "$C/wikitext-2-raw/wiki.train.raw" | awk '{printf "   %s  %.1f MB\n", $9, $5/1048576}'
else
  echo "   MISSING"; fi

echo "== Redis source, concatenated (gitcmp) =="
if [ -d "$C/redis" ]; then
  # same shape the git side will commit: the C sources of the codebase
  find "$C/redis/src" -name '*.c' -o -name '*.h' | sort | xargs cat > "$C/redis_src.txt"
  ls -la "$C/redis_src.txt" | awk '{printf "   %s  %.1f MB\n", $9, $5/1048576}'
  echo "   files: $(find "$C/redis/src" -name '*.c' -o -name '*.h' | wc -l)"
  echo "   redis version: $(git -C "$C/redis" describe --tags 2>/dev/null || git -C "$C/redis" rev-parse --short HEAD)"
else
  echo "   MISSING"; fi

echo "== H.264 elementary stream (vbench) =="
if [ ! -f "$C/vid/sample.264" ]; then
  # a real encode, not a synthetic byte pattern: vbench needs genuine IDR
  # boundaries to cut on. 1 IDR per second gives plenty of legal cut points.
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=640x480:rate=30:duration=90 \
    -c:v libx264 -preset veryfast -g 30 -keyint_min 30 -sc_threshold 0 \
    -pix_fmt yuv420p -f h264 "$C/vid/sample.264" || echo "   ffmpeg FAILED"
fi
if [ -f "$C/vid/sample.264" ]; then
  ls -la "$C/vid/sample.264" | awk '{printf "   %s  %.1f MB\n", $9, $5/1048576}'
  echo "   IDR count: $(ffprobe -hide_banner -loglevel error -select_streams v \
      -show_entries frame=key_frame -of csv=p=0 "$C/vid/sample.264" 2>/dev/null | grep -c '^1')"
  echo "   frames:    $(ffprobe -hide_banner -loglevel error -select_streams v \
      -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$C/vid/sample.264" 2>/dev/null)"
fi
