#!/usr/bin/env bash
# Sample a Steam appmanifest's progress counters twice so you can see
# which one actually moves during a download. Steam splits a download
# into a network/decompress phase (BytesToDownload/BytesDownloaded) and a
# disk/staging phase (BytesToStage/BytesStaged); the field that climbs
# between the two samples is the real progress source the led daemon
# should read for the steam_download bar.
#
# usage: tools/steam-manifest-watch.sh [appid] [seconds-between-samples]
#   appid    a specific app (e.g. 391220); defaults to every manifest found
#   seconds  delay between the two samples (default 15)
set -u

appid="${1:-}"
delay="${2:-15}"

# steamapps lives under per-user Steam roots and any external-drive
# libraries; cover the same spots host/steam.hpp scans plus the usual
# removable-media mount points
roots=(/home /var/home /mnt /run/media /media)

mapfile -t manifests < <(find "${roots[@]}" -type f \
    -name "appmanifest_${appid:-*}.acf" 2>/dev/null | sort -u)

if [ "${#manifests[@]}" -eq 0 ]; then
    echo "no appmanifest_${appid:-*}.acf found under: ${roots[*]}"
    echo "(start the download first, or pass the right appid)"
    exit 1
fi

fields='"(StateFlags|BytesToDownload|BytesDownloaded|BytesToStage|BytesStaged|SizeOnDisk|StagingSize)"'

sample () {
    for f in "${manifests[@]}"; do
        echo "== $f"
        grep -E "$fields" "$f"
    done
}

echo "### sample 1 ($(date +%T))"
sample
echo
echo "### waiting ${delay}s for the download to advance..."
sleep "$delay"
echo
echo "### sample 2 ($(date +%T))"
sample
echo
echo "# whichever counter increased between the samples is the real"
echo "# progress source; paste both samples back."
