#!/usr/bin/env bash
# Show every occurrence (with line numbers) of the progress/state fields
# in a Steam appmanifest, to explain why the daemon reads a 240/0
# placeholder when the file actually contains the real byte counts.
#
#   - if BytesToDownload appears on MORE THAN ONE line, the parser in
#     addManifest (which keeps the last match) is landing on the wrong
#     one -> fix: take the first/top-level occurrence or the max.
#   - if it's a SINGLE line, the value is state-dependent (Steam writes a
#     placeholder when the update isn't "running") -> fix: ignore the
#     placeholder and hold the last real reading.
#
# usage: tools/steam-manifest-fields.sh [appid]
#   appid  a specific app (e.g. 391220); defaults to every manifest found
set -u

appid="${1:-}"
roots=(/home /var/home /mnt /run/media /media)

mapfile -t manifests < <(find "${roots[@]}" -type f \
    -name "appmanifest_${appid:-*}.acf" 2>/dev/null | sort -u)

if [ "${#manifests[@]}" -eq 0 ]; then
    echo "no appmanifest_${appid:-*}.acf found under: ${roots[*]}"
    exit 1
fi

fields='"(StateFlags|BytesToDownload|BytesDownloaded|BytesToStage|BytesStaged|SizeOnDisk|StagingSize)"'

for f in "${manifests[@]}"; do
    echo "===== $f ====="
    grep -n -E "$fields" "$f"

    n="$(grep -c -E '"BytesToDownload"' "$f")"
    echo "-> BytesToDownload occurrences: $n"
    if [ "$n" -gt 1 ]; then
        echo "   (DUPLICATE key -> the daemon's last-match parser picks the wrong one)"
    fi
    echo
done
