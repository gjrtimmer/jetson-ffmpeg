#!/usr/bin/env bash
# Download a released jetson-nvmpi package (libnvmpi) and unpack it into DEST.
#
# libnvmpi lives in timmertech/jetson-nvmpi. Each release publishes
# jetson-nvmpi_<ver>_l4t<tag>_aarch64.tar.gz, holding lib/libnvmpi.so*,
# include/nvmpi.h and lib/pkgconfig/nvmpi.pc, to two places:
#   github: public release asset of gjrtimmer/jetson-nvmpi (no token needed)
#   gitlab: generic package registry of GitLab project 515 (token needed)
# Source order (--source auto): in CI (CI_JOB_TOKEN set) gitlab first, because
# the job token is allowlisted and the registry is on the runner's network;
# anywhere else github first. Either one falls back to the other when it fails
# (gitlab only when a token is available). --source github|gitlab forces one.
# Integrity: sha256 from the GitHub release asset digest, or from the GitLab
# package file record; when neither is readable, size + archive file list.
#
# Usage:
#   scripts/fetch-nvmpi.sh --version VER --dest DIR [options]
#
# Options:
#   --version VER   jetson-nvmpi release, e.g. 4.10.4 (default: $NVMPI_VERSION).
#   --l4t TAG       L4T tag of the package: r36.4.0 (JP6) or r35.4.1 (JP5)
#                   (default: $L4T_TAG, else r36.4.0).
#   --dest DIR      Install prefix to unpack into (lib/, include/).
#   --source SRC    auto (default, or $NVMPI_SOURCE), github or gitlab.
#   --token TOKEN   GitLab API token (default: $CI_JOB_TOKEN in CI, else
#                   $GITLAB_TOKEN). Never printed, never sent to GitHub.
#   -h, --help      Show this help.
#
# Env: CI_API_V4_URL (default https://gitlab.com/api/v4), NVMPI_PROJECT_ID (515),
#      NVMPI_GITHUB_REPO (gjrtimmer/jetson-nvmpi).
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
GH_REPO="${NVMPI_GITHUB_REPO:-gjrtimmer/jetson-nvmpi}"
SOURCE="${NVMPI_SOURCE:-auto}"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift ;;
        --l4t)     L4T="$2"; shift ;;
        --dest)    DEST="$2"; shift ;;
        --source)  SOURCE="$2"; shift ;;
        --token)   TOKEN="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done
[ -n "${VERSION}" ] || { echo "[E] --version (or NVMPI_VERSION) is required" >&2; exit 1; }
[ -n "${DEST}" ]    || { echo "[E] --dest is required" >&2; exit 1; }
case "${SOURCE}" in
    auto|github|gitlab) ;;
    *) echo "[E] --source must be auto, github or gitlab" >&2; exit 1 ;;
esac

# GitLab auth header: job token in CI (jetson-ffmpeg is on the jetson-nvmpi
# allowlist), personal/project token for manual runs. It goes through a 0600
# config file so the value never appears in the process list or the log.
HDR=""
if [ -n "${TOKEN}" ]; then HDR="PRIVATE-TOKEN: ${TOKEN}"
elif [ -n "${CI_JOB_TOKEN:-}" ]; then HDR="JOB-TOKEN: ${CI_JOB_TOKEN}"
elif [ -n "${GITLAB_TOKEN:-}" ]; then HDR="PRIVATE-TOKEN: ${GITLAB_TOKEN}"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
umask 077

# fetch URL OUTFILE [auth]: curl if present, else wget (the CI builder images
# ship wget only). The GitLab header is sent only with a third arg "auth", so
# a GitLab token can never reach github.com.
fetch() {
    local auth="${HDR}"
    [ "${3:-}" = auth ] || auth=""
    if command -v curl >/dev/null 2>&1; then
        if [ -n "${auth}" ]; then
            printf 'header = "%s"\n' "${auth}" > "${TMP}/curl.cfg"
            curl -fsSL --retry 3 -K "${TMP}/curl.cfg" -o "$2" "$1"
        else
            curl -fsSL --retry 3 -o "$2" "$1"
        fi
    else
        if [ -n "${auth}" ]; then
            printf 'header = %s\n' "${auth}" > "${TMP}/wget.cfg"
            WGETRC="${TMP}/wget.cfg" wget -q -t 3 -O "$2" "$1"
        else
            wget -q -t 3 -O "$2" "$1"
        fi
    fi
}

# jget FILE KIND KEY: print one value from a JSON file; jq, else python3, else nothing.
#   pkg KEY  GitLab package list: id of the package with version KEY
#   sha KEY  GitLab package files: file_sha256 of file_name KEY
#   gh  KEY  GitHub release: sha256 hex of the digest of the asset named KEY
jget() {
    local f="$1" kind="$2" key="$3"
    if command -v jq >/dev/null 2>&1; then
        case "${kind}" in
            pkg) jq -r --arg v "${key}" 'map(select(.version==$v))[0].id // empty' "${f}" ;;
            sha) jq -r --arg f "${key}" 'map(select(.file_name==$f))[0].file_sha256 // empty' "${f}" ;;
            gh)  jq -r --arg f "${key}" '(.assets|map(select(.name==$f))[0].digest // "") | ltrimstr("sha256:")' "${f}" ;;
        esac
    elif command -v python3 >/dev/null 2>&1; then
        python3 -I - "${f}" "${kind}" "${key}" <<'PY'
import json, sys
f, kind, key = sys.argv[1:4]
d = json.load(open(f))
if kind == "gh":
    for a in d.get("assets", []):
        if a.get("name") == key:
            print((a.get("digest") or "").replace("sha256:", "", 1)); break
else:
    for r in d:
        if kind == "pkg" and r.get("version") == key:
            print(r["id"]); break
        if kind == "sha" and r.get("file_name") == key:
            print(r.get("file_sha256") or ""); break
PY
    fi
}

FILE="jetson-nvmpi_${VERSION}_l4t${L4T}_aarch64.tar.gz"
OUT="${TMP}/${FILE}"
EXPECT=""

# Each source downloads FILE to OUT and sets EXPECT (sha256 or empty). A
# non-zero return lets the caller try the next source.
from_github() {
    fetch "https://github.com/${GH_REPO}/releases/download/v${VERSION}/${FILE}" "${OUT}" || return 1
    # Asset digest from the releases API (unauthenticated, best effort).
    if fetch "https://api.github.com/repos/${GH_REPO}/releases/tags/v${VERSION}" "${TMP}/rel.json" 2>/dev/null; then
        EXPECT="$(jget "${TMP}/rel.json" gh "${FILE}" 2>/dev/null || true)"
    fi
}

from_gitlab() {
    local base="${API}/projects/${PROJECT}/packages/generic/jetson-nvmpi/${VERSION}" pkg_id
    [ -n "${HDR}" ] || { echo "[w] gitlab source needs a token (CI_JOB_TOKEN, GITLAB_TOKEN or --token)" >&2; return 1; }
    fetch "${base}/${FILE}" "${OUT}" auth || return 1
    # sha256 as recorded by the registry (package_files API); the job token may
    # not be allowed to list packages.
    if fetch "${API}/projects/${PROJECT}/packages?package_name=jetson-nvmpi&package_version=${VERSION}" "${TMP}/pkg.json" auth 2>/dev/null; then
        pkg_id="$(jget "${TMP}/pkg.json" pkg "${VERSION}" 2>/dev/null || true)"
        if [ -n "${pkg_id}" ] && fetch "${API}/projects/${PROJECT}/packages/${pkg_id}/package_files?per_page=100" "${TMP}/files.json" auth 2>/dev/null; then
            EXPECT="$(jget "${TMP}/files.json" sha "${FILE}" 2>/dev/null || true)"
        fi
    fi
}

case "${SOURCE}" in
    github) ORDER="github" ;;
    gitlab) ORDER="gitlab" ;;
    *) if [ -n "${CI_JOB_TOKEN:-}" ]; then ORDER="gitlab github"; else ORDER="github gitlab"; fi ;;
esac

GOT=""
for src in ${ORDER}; do
    echo "[i] fetching ${FILE} (jetson-nvmpi ${VERSION}) from ${src}"
    EXPECT=""
    rm -f "${OUT}"
    if "from_${src}" && [ -s "${OUT}" ]; then GOT="${src}"; break; fi
    echo "[w] ${src} fetch failed" >&2
done
[ -n "${GOT}" ] || { echo "[E] could not fetch ${FILE} (tried: ${ORDER})" >&2; exit 1; }

if [ -n "${EXPECT}" ]; then
    echo "${EXPECT}  ${OUT}" | sha256sum -c - >/dev/null \
        || { echo "[E] sha256 mismatch for ${FILE} (${GOT})" >&2; exit 1; }
    echo "[i] sha256 verified against the ${GOT} record"
else
    echo "[w] no sha256 readable from ${GOT}; relying on archive sanity checks" >&2
fi

# Sanity: valid archive that carries the library and the header.
tar -tzf "${OUT}" > "${TMP}/list.txt"
for need in lib/libnvmpi.so lib/libnvmpi.so.1 include/nvmpi.h; do
    grep -q "/${need}\$" "${TMP}/list.txt" || { echo "[E] ${FILE} lacks ${need}" >&2; exit 1; }
done

mkdir -p "${DEST}"
tar -xzf "${OUT}" --strip-components=1 -C "${DEST}"
test -e "${DEST}/lib/libnvmpi.so.1"
echo "[i] libnvmpi ${VERSION} (${L4T}) unpacked into ${DEST}"
