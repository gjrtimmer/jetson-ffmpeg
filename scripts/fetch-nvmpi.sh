#!/usr/bin/env bash
# Download a released jetson-nvmpi package (libnvmpi) and unpack it into DEST.
#
# libnvmpi lives in timmertech/jetson-nvmpi (GitLab project 515). Each release
# publishes jetson-nvmpi_<ver>_l4t<tag>_aarch64.tar.gz to the generic package
# registry, holding lib/libnvmpi.so*, include/nvmpi.h and lib/pkgconfig/nvmpi.pc.
# The archive is verified (sha256 from the registry when it can be read, else
# size + file list) before it is unpacked.
#
# Usage:
#   scripts/fetch-nvmpi.sh --version VER --dest DIR [options]
#
# Options:
#   --version VER   jetson-nvmpi release, e.g. 4.0.0 (default: $NVMPI_VERSION).
#   --l4t TAG       L4T tag of the package: r36.4.0 (JP6) or r35.4.1 (JP5)
#                   (default: $L4T_TAG, else r36.4.0).
#   --dest DIR      Install prefix to unpack into (lib/, include/).
#   --token TOKEN   API token for a manual run (default: $CI_JOB_TOKEN in CI,
#                   else $GITLAB_TOKEN). The token is never printed.
#   -h, --help      Show this help.
#
# Env: CI_API_V4_URL (default https://gitlab.com/api/v4), NVMPI_PROJECT_ID (515).
#
# For a deb install on a Jetson, use the libnvmpi1 / libnvmpi-dev packages of
# the same release instead; this script is for CI and local prefix builds.
set -euo pipefail

VERSION="${NVMPI_VERSION:-}"
L4T="${L4T_TAG:-r36.4.0}"
DEST=""
TOKEN=""
API="${CI_API_V4_URL:-https://gitlab.com/api/v4}"
PROJECT="${NVMPI_PROJECT_ID:-515}"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift ;;
        --l4t)     L4T="$2"; shift ;;
        --dest)    DEST="$2"; shift ;;
        --token)   TOKEN="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done
[ -n "${VERSION}" ] || { echo "[E] --version (or NVMPI_VERSION) is required" >&2; exit 1; }
[ -n "${DEST}" ]    || { echo "[E] --dest is required" >&2; exit 1; }

# Auth header: job token in CI (jetson-ffmpeg is on the jetson-nvmpi allowlist),
# personal/project token for manual runs. It goes through a 0600 config file so
# the value never appears in the process list or the log.
HDR=""
if [ -n "${TOKEN}" ]; then HDR="PRIVATE-TOKEN: ${TOKEN}"
elif [ -n "${CI_JOB_TOKEN:-}" ]; then HDR="JOB-TOKEN: ${CI_JOB_TOKEN}"
elif [ -n "${GITLAB_TOKEN:-}" ]; then HDR="PRIVATE-TOKEN: ${GITLAB_TOKEN}"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
umask 077

# fetch URL OUTFILE: curl if present, else wget (the CI builder images ship wget only)
fetch() {
    if command -v curl >/dev/null 2>&1; then
        if [ -n "${HDR}" ]; then
            printf 'header = "%s"\n' "${HDR}" > "${TMP}/curl.cfg"
            curl -fsSL --retry 3 -K "${TMP}/curl.cfg" -o "$2" "$1"
        else
            curl -fsSL --retry 3 -o "$2" "$1"
        fi
    else
        if [ -n "${HDR}" ]; then
            printf 'header = %s\n' "${HDR}" > "${TMP}/wget.cfg"
            WGETRC="${TMP}/wget.cfg" wget -q -t 3 -O "$2" "$1"
        else
            wget -q -t 3 -O "$2" "$1"
        fi
    fi
}

# jget FILE FILTER-ARGS...: jq when present, else python3, else nothing.
# Usage: jget file pkg VERSION | jget file sha FILENAME
jget() {
    local f="$1" kind="$2" key="$3"
    if command -v jq >/dev/null 2>&1; then
        if [ "${kind}" = pkg ]; then
            jq -r --arg v "${key}" 'map(select(.version==$v))[0].id // empty' "${f}"
        else
            jq -r --arg f "${key}" 'map(select(.file_name==$f))[0].file_sha256 // empty' "${f}"
        fi
    elif command -v python3 >/dev/null 2>&1; then
        python3 -I - "${f}" "${kind}" "${key}" <<'PY'
import json, sys
f, kind, key = sys.argv[1:4]
rows = json.load(open(f))
for r in rows:
    if kind == "pkg" and r.get("version") == key:
        print(r["id"]); break
    if kind == "sha" and r.get("file_name") == key:
        print(r.get("file_sha256") or ""); break
PY
    fi
}

FILE="jetson-nvmpi_${VERSION}_l4t${L4T}_aarch64.tar.gz"
BASE="${API}/projects/${PROJECT}/packages/generic/jetson-nvmpi/${VERSION}"

echo "[i] fetching ${FILE} (jetson-nvmpi ${VERSION})"
fetch "${BASE}/${FILE}" "${TMP}/${FILE}"
[ -s "${TMP}/${FILE}" ] || { echo "[E] empty download: ${FILE}" >&2; exit 1; }

# sha256 as recorded by the registry (package_files API); best effort, the job
# token may not be allowed to list packages.
EXPECT=""
if fetch "${API}/projects/${PROJECT}/packages?package_name=jetson-nvmpi&package_version=${VERSION}" "${TMP}/pkg.json" 2>/dev/null; then
    PKG_ID="$(jget "${TMP}/pkg.json" pkg "${VERSION}" 2>/dev/null || true)"
    if [ -n "${PKG_ID}" ] && fetch "${API}/projects/${PROJECT}/packages/${PKG_ID}/package_files?per_page=100" "${TMP}/files.json" 2>/dev/null; then
        EXPECT="$(jget "${TMP}/files.json" sha "${FILE}" 2>/dev/null || true)"
    fi
fi
if [ -n "${EXPECT}" ]; then
    echo "${EXPECT}  ${TMP}/${FILE}" | sha256sum -c - >/dev/null \
        || { echo "[E] sha256 mismatch for ${FILE}" >&2; exit 1; }
    echo "[i] sha256 verified against the package registry"
else
    echo "[w] registry sha256 not readable; relying on archive sanity checks" >&2
fi

# Sanity: valid archive that carries the library and the header.
tar -tzf "${TMP}/${FILE}" > "${TMP}/list.txt"
for need in lib/libnvmpi.so lib/libnvmpi.so.1 include/nvmpi.h; do
    grep -q "/${need}\$" "${TMP}/list.txt" || { echo "[E] ${FILE} lacks ${need}" >&2; exit 1; }
done

mkdir -p "${DEST}"
tar -xzf "${TMP}/${FILE}" --strip-components=1 -C "${DEST}"
test -e "${DEST}/lib/libnvmpi.so.1"
echo "[i] libnvmpi ${VERSION} (${L4T}) unpacked into ${DEST}"
