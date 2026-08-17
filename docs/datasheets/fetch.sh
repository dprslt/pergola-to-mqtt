#!/usr/bin/env bash
# Fetch the datasheets this project references.
#
# They are not committed: they are the manufacturers' documents, not ours.
# The notes in docs/cc1101/ cite page numbers of the revision below.
set -euo pipefail

cd "$(dirname "$0")"

fetch() {
	local name=$1 url=$2
	if [[ -f $name ]]; then
		echo "have    $name"
		return
	fi
	echo "fetch   $name"
	curl -fsSL --retry 3 -o "$name.part" "$url"
	mv "$name.part" "$name"
}

# CC1101 Low-Power Sub-1 GHz RF Transceiver, revision SWRS061I, 98 pages.
# docs/cc1101/*.md cite page numbers of THIS revision. If TI publishes a newer
# one the page numbers may drift.
fetch cc1101-swrs061i.pdf https://www.ti.com/lit/ds/symlink/cc1101.pdf

echo
echo "Design notes worth reading, not auto-fetched (they sit behind ti.com search):"
echo "  DN022 - CC11xx OOK/ASK register settings   <- the one that matters here"
echo "  DN010 - close-in reception with CC1101"
echo "  DN005 - CC11xx sensitivity vs frequency offset"
echo "  DN505 - RSSI interpretation and timing"
echo "  DN501 - PATABLE access"
