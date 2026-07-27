#!/usr/bin/env bash
#
# Downloads a day of NASDAQ TotalView-ITCH 5.0 from the public sample archive.
#
# The repository commits only a single symbol slice, because these files are 3.5 to
# 4.8 GB compressed and GitHub rejects anything over 100 MB. This script is how the
# full capture is reproduced, and it is the difference between a benchmark number
# someone can rerun and one they have to trust.
#
# It fetches in ranged chunks rather than as one request, and that is not
# incidental. A single sustained request to this host was measured collapsing to
# about 30 KB/s, which would take over a day, while ranged requests hold 4 MB/s.
# Chunking also makes the download resumable: rerun after an interruption and it
# picks up from the first chunk it does not already have.

set -uo pipefail

DATE="${1:-12302019}"
OUT_DIR="${2:-data}"
BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
ARCHIVE="${DATE}.NASDAQ_ITCH50.gz"
URL="${BASE_URL}/${ARCHIVE}"

CHUNK_BYTES="${CHUNK_BYTES:-268435456}"  # 256 MiB
PREFIX_ONLY=0
DECOMPRESS=1

usage() {
  cat <<'USAGE'
usage: fetch_nasdaq_sample.sh [date] [output-dir] [--prefix-only] [--keep-gz]

  date          MMDDYYYY, default 12302019. Available as of 2026:
                01302018 03292018 05302018 07302018 08302018 10302018 12282018
                01302019 03272019 05302019 07302019 08302019 10302019 12302019
                01302020
  output-dir    default data/
  --prefix-only fetch only the first chunk, which contains the full symbol
                directory plus the market open and is enough to exercise the
                parser. A gzip stream decompresses correctly up to the point it is
                cut, so the parser reports truncated_message at the end, which is
                the correct answer rather than an error to work around.
  --keep-gz     leave the compressed archive in place after decompressing

Roughly 3.5 to 4.8 GB compressed and 10 to 15 GB decompressed, so budget the disk.
Interrupted runs resume: rerun the same command.
USAGE
}

for arg in "$@"; do
  case "${arg}" in
    --help|-h) usage; exit 0 ;;
    --prefix-only) PREFIX_ONLY=1 ;;
    --keep-gz) DECOMPRESS=2 ;;
  esac
done

mkdir -p "${OUT_DIR}"
TARGET_GZ="${OUT_DIR}/${ARCHIVE}"
TARGET="${OUT_DIR}/${DATE}.NASDAQ_ITCH50"

echo "querying ${ARCHIVE}"
TOTAL=$(curl -sIL "${URL}" | tr -d '\r' | awk 'tolower($1) == "content-length:" {print $2}' | tail -1)
if [[ -z "${TOTAL}" ]]; then
  echo "could not determine the size of ${URL}" >&2
  exit 1
fi
echo "  ${TOTAL} bytes"

if [[ "${PREFIX_ONLY}" -eq 1 ]]; then
  TOTAL="${CHUNK_BYTES}"
  echo "  prefix mode: fetching the first ${CHUNK_BYTES} bytes only"
fi

# Resume from whatever is already on disk, rounded down to a chunk boundary so a
# partially written chunk is refetched rather than trusted.
have=0
if [[ -f "${TARGET_GZ}" ]]; then
  have=$(stat -c%s "${TARGET_GZ}")
  have=$(( (have / CHUNK_BYTES) * CHUNK_BYTES ))
  if [[ "${have}" -gt 0 ]]; then
    echo "  resuming from ${have} bytes already downloaded"
    truncate -s "${have}" "${TARGET_GZ}"
  fi
fi

start="${have}"
while [[ "${start}" -lt "${TOTAL}" ]]; do
  end=$(( start + CHUNK_BYTES - 1 ))
  [[ "${end}" -ge "${TOTAL}" ]] && end=$(( TOTAL - 1 ))
  want=$(( end - start + 1 ))

  ok=0
  for attempt in 1 2 3; do
    if curl -sL --max-time 900 -r "${start}-${end}" -o "${OUT_DIR}/.chunk.tmp" "${URL}"; then
      got=$(stat -c%s "${OUT_DIR}/.chunk.tmp" 2>/dev/null || echo 0)
      if [[ "${got}" -eq "${want}" ]]; then ok=1; break; fi
      echo "    short read on attempt ${attempt}: ${got} of ${want}"
    fi
    sleep 3
  done

  if [[ "${ok}" -ne 1 ]]; then
    echo "failed at byte ${start} after 3 attempts. Rerun to resume." >&2
    rm -f "${OUT_DIR}/.chunk.tmp"
    exit 1
  fi

  cat "${OUT_DIR}/.chunk.tmp" >> "${TARGET_GZ}"
  rm -f "${OUT_DIR}/.chunk.tmp"
  start=$(( end + 1 ))
  printf '  %s of %s bytes (%d%%)\n' "${start}" "${TOTAL}" $(( start * 100 / TOTAL ))
done

echo "decompressing"
if [[ "${PREFIX_ONLY}" -eq 1 ]]; then
  # A truncated gzip stream errors at the cut after emitting everything before it,
  # so the failure is expected and the output is usable.
  gzip -dc "${TARGET_GZ}" > "${TARGET}" 2>/dev/null || true
else
  gzip -dc "${TARGET_GZ}" > "${TARGET}"
fi

if [[ "${DECOMPRESS}" -eq 1 ]]; then
  rm -f "${TARGET_GZ}"
fi

ls -la "${TARGET}"

cat <<EOF

done. Try:

  itch_replay --survey --top 20 ${TARGET}
  itch_replay --symbol QQQ ${TARGET}
  itch_replay --symbol QQQ --extract data/qqq_slice.itch ${TARGET}

To publish the full archive as GitHub release assets:

  scripts/publish_dataset.sh ${TARGET}

EOF
