#!/usr/bin/env bash
# Figure out what actually tracks a Steam download's progress on this
# box. The led daemon reads BytesToDownload/BytesDownloaded from the
# appmanifest, but Steam doesn't always keep those live: it streams
# in-flight chunks into steamapps/downloading/<appid>/ and only folds
# them into the manifest periodically, so the .acf can sit frozen while
# the download is moving.
#
# This dumps the full manifest once, then samples both the manifest
# counters and the download-cache directory size twice, so we can see
# which (if any) actually advances during a download.
#
# usage: tools/steam-manifest-watch.sh [appid] [seconds-between-samples]
#   appid    a specific app (e.g. 391220); defaults to every manifest found
#   seconds  delay between the two samples (default 15)
set -u

appid="${1:-}"
delay="${2:-15}"

roots=(/home /var/home /mnt /run/media /media)

mapfile -t manifests < <(find "${roots[@]}" -type f \
    -name "appmanifest_${appid:-*}.acf" 2>/dev/null | sort -u)

if [ "${#manifests[@]}" -eq 0 ]; then
    echo "no appmanifest_${appid:-*}.acf found under: ${roots[*]}"
    echo "(start the download first, or pass the right appid)"
    exit 1
fi

counters='"(StateFlags|BytesToDownload|BytesDownloaded|BytesToStage|BytesStaged|SizeOnDisk|StagingSize)"'

# steamapps/downloading/<id> holds the in-flight chunks; its size is the
# live signal even when the manifest is frozen
cache_dir () {
    local f="$1" id
    id="$(basename "$f" .acf)"; id="${id#appmanifest_}"
    echo "$(dirname "$f")/downloading/$id"
}

sample () {
    for f in "${manifests[@]}"; do
        echo "-- $f"
        grep -E "$counters" "$f"
        local dl; dl="$(cache_dir "$f")"
        if [ -d "$dl" ]; then
            echo "   downloading-cache: $(du -sh "$dl" 2>/dev/null | cut -f1) ($dl)"
        else
            echo "   downloading-cache: (no dir $dl)"
        fi
    done
}

echo "########## full manifest(s) ##########"
for f in "${manifests[@]}"; do
    echo "===== $f ====="
    cat "$f"
    echo
done

echo "########## sample 1 ($(date +%T)) ##########"
sample
echo
echo "### waiting ${delay}s for the download to advance..."
sleep "$delay"
echo
echo "########## sample 2 ($(date +%T)) ##########"
sample
echo
echo "# whichever number moved between samples (a manifest counter or the"
echo "# downloading-cache size) is what the daemon should track; paste it all back."
