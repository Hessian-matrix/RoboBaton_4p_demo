#!/bin/sh
set -eu

if [ "$#" -eq 1 ] && [ "$1" = "-version" ]; then
  printf '%s\n' "ffprobe-frame-count-helper 1.0 (uses ffmpeg)"
  exit 0
fi

input=""
count_frames_seen=0
show_entries=""
output_format=""
select_streams=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    -v)
      shift
      [ "$#" -gt 0 ] || { printf '%s\n' "ffprobe helper: -v requires a value" >&2; exit 2; }
      shift
      ;;
    -select_streams)
      shift
      [ "$#" -gt 0 ] || { printf '%s\n' "ffprobe helper: -select_streams requires a value" >&2; exit 2; }
      select_streams="$1"
      shift
      ;;
    -count_frames)
      count_frames_seen=1
      shift
      ;;
    -show_entries)
      shift
      [ "$#" -gt 0 ] || { printf '%s\n' "ffprobe helper: -show_entries requires a value" >&2; exit 2; }
      show_entries="$1"
      shift
      ;;
    -of)
      shift
      [ "$#" -gt 0 ] || { printf '%s\n' "ffprobe helper: -of requires a value" >&2; exit 2; }
      output_format="$1"
      shift
      ;;
    --)
      shift
      [ "$#" -eq 1 ] || { printf '%s\n' "ffprobe helper: expected one input after --" >&2; exit 2; }
      input="$1"
      shift
      ;;
    -*)
      printf '%s\n' "ffprobe helper: unsupported option $1" >&2
      exit 2
      ;;
    *)
      [ -z "$input" ] || { printf '%s\n' "ffprobe helper: multiple inputs are not supported" >&2; exit 2; }
      input="$1"
      shift
      ;;
  esac
done

[ "$count_frames_seen" -eq 1 ] || { printf '%s\n' "ffprobe helper: -count_frames is required" >&2; exit 2; }
[ "$select_streams" = "v:0" ] || { printf '%s\n' "ffprobe helper: only -select_streams v:0 is supported" >&2; exit 2; }
[ "$show_entries" = "stream=nb_read_frames" ] || { printf '%s\n' "ffprobe helper: only stream=nb_read_frames is supported" >&2; exit 2; }
case "$output_format" in
  default=noprint_wrappers=1:nokey=1|default=nokey=1:noprint_wrappers=1) ;;
  *)
    printf '%s\n' "ffprobe helper: only default nokey output is supported" >&2
    exit 2
    ;;
esac
[ -n "$input" ] && [ -f "$input" ] || { printf '%s\n' "ffprobe helper: input file is missing" >&2; exit 2; }

ffmpeg_bin="$(command -v ffmpeg || true)"
[ -n "$ffmpeg_bin" ] && [ -x "$ffmpeg_bin" ] || { printf '%s\n' "ffprobe helper: ffmpeg executable not found in PATH" >&2; exit 127; }

work_dir="$(dirname "$input")"
tmp="$work_dir/.ffprobe-frame-count-$$.mp4"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
set +e
output="$($ffmpeg_bin -hide_banner -nostdin -stats -i "$input" -map 0:v:0 -c:v copy -movflags frag_keyframe+empty_moov -f mp4 -y "$tmp" 2>&1)"
rc=$?
set -e
rm -f "$tmp"
trap - EXIT HUP INT TERM
if [ "$rc" -ne 0 ]; then
  printf '%s\n' "$output" >&2
  exit "$rc"
fi

frame="$(printf '%s\n' "$output" | sed -n 's/.*frame=[[:space:]]*\([0-9][0-9]*\).*/\1/p' | sed -n '$p')"
[ -n "$frame" ] || { printf '%s\n' "ffprobe helper: frame count unavailable" >&2; exit 1; }
printf '%s\n' "$frame"
