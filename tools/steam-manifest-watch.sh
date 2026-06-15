#!/usr/bin/env bash
# Steam's appmanifest doesn't always record byte-level progress (we've
# seen StateFlags "started" but not "running", with a frozen 240/0
# placeholder). So instead of trusting the manifest, measure what a
# download actually does: bytes arriving on the network, and bytes
# landing in steamapps/downloading/<appid>/ -- in EXACT bytes, so small
# movement isn't hidden by rounding.
#
# Run it during the download. If network RX climbs by hundreds of MB,
# a transfer is genuinely live and we drive the led effect off that. If
# everything is flat, the download isn't actually transferring right now
# (queued/stalled), which is a Steam-side thing, not a daemon bug.
#
# usage: tools/steam-manifest-watch.sh [appid] [seconds-between-samples]
set -u

appid="${1:-}"
delay="${2:-20}"

# total received bytes across real (non-loopback) interfaces
rx_total () {
    local sum=0 b s
    for s in /sys/class/net/*/statistics/rx_bytes; do
        case "$s" in */lo/*) continue ;; esac
        b="$(cat "$s" 2>/dev/null || echo 0)"
        sum=$((sum + b))
    done
    echo "$sum"
}

roots=(/home /var/home /mnt /run/media /media)
mapfile -t manifests < <(find "${roots[@]}" -type f \
    -name "appmanifest_${appid:-*}.acf" 2>/dev/null | sort -u)

# exact byte size of steamapps/downloading/<id> for a given manifest
cache_bytes () {
    local f="$1" id dl
    id="$(basename "$f" .acf)"; id="${id#appmanifest_}"
    dl="$(dirname "$f")/downloading/$id"
    [ -d "$dl" ] && du -sb "$dl" 2>/dev/null | cut -f1 || echo "-"
}

echo "interfaces:"
for s in /sys/class/net/*/statistics/rx_bytes; do
    case "$s" in */lo/*) continue ;; esac
    iface="$(basename "$(dirname "$(dirname "$s")")")"
    echo "  $iface rx=$(cat "$s")"
done

rx1="$(rx_total)"
declare -A c1
for f in "${manifests[@]}"; do c1["$f"]="$(cache_bytes "$f")"; done

echo "sampling over ${delay}s (keep the download going)..."
sleep "$delay"

rx2="$(rx_total)"

echo
awk -v d="$((rx2 - rx1))" -v t="$delay" \
    'BEGIN { printf "network RX: %d bytes in %ss = %.2f MB/s\n", d, t, d/t/1048576 }'

for f in "${manifests[@]}"; do
    c2="$(cache_bytes "$f")"
    echo "download-cache $(basename "$f"): ${c1[$f]} -> ${c2} bytes"
done

echo
echo "# RX climbing (MB/s) => use network as the signal."
echo "# cache bytes climbing => use the download-cache size."
echo "# both flat => nothing is actually transferring (Steam-side, not the daemon)."
