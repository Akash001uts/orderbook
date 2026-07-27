#!/usr/bin/env bash
#
# Downloads a full day of NASDAQ TotalView-ITCH 5.0 from the public sample archive.
#
# The repository commits only a single symbol slice, because these files are 3.5 to
# 4.8 GB compressed and GitHub rejects anything over 100 MB. This script is how the
# full capture is reproduced, and it is the difference between a benchmark number
# someone can rerun and one they have to trust.

set -euo pipefail

DATE="${1:-12302019}"
OUT_DIR="${2:-data}"
BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
ARCHIVE="${DATE}.NASDAQ_ITCH50.gz"
TARGET="${OUT_DIR}/${DATE}.NASDAQ_ITCH50"

usage() {
  cat <<'USAGE'
usage: fetch_nasdaq_sample.sh [date] [output-dir]

  date        MMDDYYYY, default 12302019. Available dates as of 2026:
              01302018 03292018 05302018 07302018 08302018 10302018 12282018
              01302019 03272019 05302019 07302019 08302019 10302019 12302019
              01302020
  output-dir  default data/

Roughly 3.5 to 4.8 GB compressed and 10 to 15 GB decompressed, so budget the disk
and the time. Pass --prefix-only to fetch just the first gigabyte, which contains
the full symbol directory plus the market open and is enough to exercise the parser.
USAGE
}

if [[ "${DATE}" == "--help" || "${DATE}" == "-h" ]]; then
  usage
  exit 0
fi

PREFIX_ONLY=0
for arg in "$@"; do
  if [[ "${arg}" == "--prefix-only" ]]; then
    PREFIX_ONLY=1
  fi
done

mkdir -p "${OUT_DIR}"

echo "fetching ${ARCHIVE}"
if [[ "${PREFIX_ONLY}" -eq 1 ]]; then
  # A gzip stream decompresses correctly up to the point it is cut, so a ranged
  # request yields a usable prefix. The parser reports truncated_message at the cut,
  # which is the correct answer rather than an error to work around.
  echo "prefix mode: first 1 GiB only"
  curl -fL --retry 3 -r 0-1073741823 -o "${TARGET}.gz.partial" "${BASE_URL}/${ARCHIVE}"
  gzip -dc "${TARGET}.gz.partial" > "${TARGET}" 2>/dev/null || true
  rm -f "${TARGET}.gz.partial"
else
  curl -fL --retry 3 -o "${TARGET}.gz" "${BASE_URL}/${ARCHIVE}"
  echo "decompressing"
  gzip -d "${TARGET}.gz"
fi

ls -la "${TARGET}"

cat <<EOF

done. Try:

  itch_replay --survey --top 20 ${TARGET}
  itch_replay --symbol QQQ ${TARGET}
  itch_replay --symbol QQQ --extract data/qqq_slice.itch ${TARGET}

EOF
