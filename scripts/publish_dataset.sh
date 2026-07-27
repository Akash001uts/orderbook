#!/usr/bin/env bash
#
# Publishes a full NASDAQ TotalView-ITCH capture as GitHub release assets.
#
# Why releases rather than the repository. Git rejects any file over 100 MB at push
# time, and that limit is not negotiable by compressing harder: measured on this
# data, xz beats the gzip NASDAQ ships by 38 percent, which takes a 3.5 GB archive
# to roughly 2.5 GB and leaves it 25 times over the limit. Release assets are the
# mechanism GitHub provides for exactly this: 2 GB per asset, they appear on the
# repository page, and they are not part of the git history so a clone stays small.
#
# The repository keeps a committed single symbol slice for CI, this script ships the
# complete dataset alongside it, and neither one makes the other redundant.
#
# Licensing. NASDAQ publishes these files publicly with no login, but publishing a
# complete copy is wholesale redistribution rather than a derived excerpt. Confirm
# their current terms before running this on a public repository. It is your call
# and it is not the same call as committing a small slice.

set -euo pipefail

SOURCE="${1:-}"
TAG="${2:-dataset-20191230}"
REPO="${3:-Akash001uts/orderbook}"

# GitHub caps a single release asset at 2 GB. Splitting below that leaves headroom
# and makes a failed upload cheap to retry.
PART_SIZE="${PART_SIZE:-1800M}"

usage() {
  cat <<'USAGE'
usage: publish_dataset.sh <capture-file> [tag] [repo]

  capture-file  the decompressed .NASDAQ_ITCH50 file, or the .gz as downloaded
  tag           release tag, default dataset-20191230
  repo          owner/name, default Akash001uts/orderbook

Recompresses with xz, splits into parts under the 2 GB asset limit, checksums
every part, and uploads them to a GitHub release along with a manifest describing
how to reassemble.

Requires the gh CLI, authenticated. Dry run first with PUBLISH_DRY_RUN=1.
USAGE
}

if [[ -z "${SOURCE}" || "${SOURCE}" == "--help" || "${SOURCE}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ ! -f "${SOURCE}" ]]; then
  echo "no such file: ${SOURCE}" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

BASE="$(basename "${SOURCE}")"
ARCHIVE="${WORK_DIR}/${BASE}.xz"

echo "recompressing ${BASE} with xz, this is slow and it is the point"
if [[ "${SOURCE}" == *.gz ]]; then
  # Recompressing an already gzipped file gains nothing. Decompress first so xz
  # sees the actual structure.
  gzip -dc "${SOURCE}" | xz -9 -T0 -c > "${ARCHIVE}"
else
  xz -9 -T0 -c "${SOURCE}" > "${ARCHIVE}"
fi

ORIGINAL_SIZE=$(stat -c%s "${SOURCE}")
ARCHIVE_SIZE=$(stat -c%s "${ARCHIVE}")
echo "  ${ORIGINAL_SIZE} bytes in, ${ARCHIVE_SIZE} bytes out"

echo "splitting into ${PART_SIZE} parts"
split -b "${PART_SIZE}" -d "${ARCHIVE}" "${WORK_DIR}/${BASE}.xz.part"

MANIFEST="${WORK_DIR}/MANIFEST.txt"
{
  echo "# NASDAQ TotalView-ITCH 5.0 capture, published as split release assets."
  echo "#"
  echo "# Source:      https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
  echo "# Original:    ${BASE}, ${ORIGINAL_SIZE} bytes"
  echo "# Recompressed: xz -9, ${ARCHIVE_SIZE} bytes"
  echo "#"
  echo "# Reassemble:"
  echo "#   cat ${BASE}.xz.part* > ${BASE}.xz"
  echo "#   xz -d ${BASE}.xz"
  echo "#"
  echo "# Then:"
  echo "#   itch_replay --survey <file>"
  echo "#   itch_replay --symbol QQQ <file>"
  echo ""
  echo "# sha256 of each part"
  ( cd "${WORK_DIR}" && sha256sum "${BASE}".xz.part* )
  echo ""
  echo "# sha256 of the reassembled archive"
  ( cd "${WORK_DIR}" && sha256sum "${BASE}.xz" )
} > "${MANIFEST}"

cat "${MANIFEST}"

if [[ "${PUBLISH_DRY_RUN:-0}" == "1" ]]; then
  echo ""
  echo "dry run, nothing uploaded. Parts are in ${WORK_DIR} until this script exits."
  exit 0
fi

echo ""
echo "creating release ${TAG} on ${REPO}"
gh release create "${TAG}" \
  --repo "${REPO}" \
  --title "NASDAQ TotalView-ITCH 5.0, 2019-12-30" \
  --notes-file - <<NOTES || echo "release ${TAG} already exists, uploading into it"
A complete NASDAQ TotalView-ITCH 5.0 capture, published as split release assets
because git rejects any file over 100 MB and compressing harder does not close a
25x gap.

Recompressed with xz, which beats the gzip NASDAQ ships by 38 percent on this data.
Download every part, concatenate, decompress:

    cat *.xz.part* > capture.xz
    xz -d capture.xz

MANIFEST.txt carries the checksums.

The repository itself holds a small single symbol slice so that its tests run with
no download at all. This is the complete dataset behind the findings in DESIGN.md.

Source: NASDAQ's public sample archive, https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
NOTES

for part in "${WORK_DIR}/${BASE}".xz.part*; do
  echo "uploading $(basename "${part}"), $(stat -c%s "${part}") bytes"
  gh release upload "${TAG}" "${part}" --repo "${REPO}" --clobber
done

gh release upload "${TAG}" "${MANIFEST}" --repo "${REPO}" --clobber

echo ""
echo "done: https://github.com/${REPO}/releases/tag/${TAG}"
