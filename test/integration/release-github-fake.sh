#!/usr/bin/env bash
# Run scripts/release-github.sh against a local fake GitHub API (python stub).
# Never talks to real GitHub. Covers: create, re-run (update + asset replace),
# a pre-existing release (the hand-made case), a URL-form GITHUB_REPO and the
# GitLab push-mirror trigger (called with a token, skipped without). No hardware needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "${SRV_PID}" ] && kill "${SRV_PID}" 2>/dev/null || true; rm -rf "${WORK}"; }
trap cleanup EXIT

# Scratch repo: scripts + a tagged commit + fake dist archives.
mkdir -p "${WORK}/repo/scripts" "${WORK}/repo/dist"
cp "${ROOT}/scripts/release-github.sh" "${ROOT}/scripts/changelog.sh" "${WORK}/repo/scripts/"
cd "${WORK}/repo"
git init -q . && git config user.email t@example.invalid && git config user.name t
echo x > f && git add f && git commit -q -m "feat: first" && git tag v9.9.9
SHA="$(git rev-parse HEAD)"
echo "repo changelog, must NOT be the asset" > CHANGELOG.md
echo a > dist/ffmpeg-7.1.tar.gz && echo b > dist/ffmpeg-9.0.tar.gz

cat > "${WORK}/stub.py" <<'PY'
import json, re, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
sha, state_file, token_log = sys.argv[2], sys.argv[3], sys.argv[4]
rel = {"id": 1, "assets": {}}  # assets: id -> name
precreated = sys.argv[5] == "1"
have = precreated
nid = [100]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj=None):
        b = json.dumps(obj if obj is not None else {}).encode()
        self.send_response(code); self.send_header("Content-Length", str(len(b))); self.end_headers()
        self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length", 0)); return self.rfile.read(n)
    def auth(self):
        open(token_log, "a").write(self.headers.get("Authorization", "") + "\n")
    def do_GET(self):
        global have
        self.auth(); p = self.path
        if "/git/ref/tags/" in p:
            assert "/repos/o/r/" in p, p  # repo normalised to owner/repo
            return self.send(200, {"object": {"type": "commit", "sha": sha}})
        if p.endswith("/releases/tags/v9.9.9"): return self.send(200, {"id": 1}) if have else self.send(404, {"message": "Not Found"})
        if "/releases/1/assets" in p:
            return self.send(200, [{"id": i, "name": n} for i, n in rel["assets"].items()])
        if p.endswith("/projects/7/remote_mirrors"):
            if self.headers.get("PRIVATE-TOKEN") != "fake-mirror-token": return self.send(401, {})
            return self.send(200, [{"id": 5, "url": "https://*****:*****@example.invalid/x.git"},
                                   {"id": 6, "url": "https://*****:*****@github.com/o/r.git"}])
        self.send(404, {"message": "unhandled " + p})
    def do_POST(self):
        global have
        self.auth(); b = self.body(); p = self.path
        if p.endswith("/projects/7/remote_mirrors/6/sync"):
            open(state_file, "a").write("mirror-sync\n"); return self.send(204)
        if p.endswith("/releases"):
            d = json.loads(b); assert d["name"] == "v9.9.9" and d["body"], d
            have = True; open(state_file, "a").write("create\n"); return self.send(201, {"id": 1})
        m = re.search(r"/releases/1/assets\?name=(.+)$", p)
        if m:
            nid[0] += 1; name = m.group(1); rel["assets"][nid[0]] = name
            if name == "CHANGELOG.md": open(state_file + ".asset", "wb").write(b)
            open(state_file, "a").write("upload " + name + "\n"); return self.send(201, {"name": name})
        self.send(404, {"message": "unhandled " + p})
    def do_PATCH(self):
        self.auth(); d = json.loads(self.body()); assert d["name"] == "v9.9.9"
        open(state_file, "a").write("patch\n"); self.send(200, {"id": 1})
    def do_DELETE(self):
        self.auth(); i = int(self.path.rsplit("/", 1)[1]); rel["assets"].pop(i)
        open(state_file, "a").write("delete\n"); self.send(204)
srv = HTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(srv.server_port))
srv.serve_forever()
PY

run_case() { # <precreated 0|1> <runs> [GITHUB_REPO value] [mirror token]
    : > "${WORK}/state"; : > "${WORK}/tokens"; rm -f "${WORK}/port"
    python3 -I "${WORK}/stub.py" "${WORK}/port" "${SHA}" "${WORK}/state" "${WORK}/tokens" "$1" &
    SRV_PID=$!
    while [ ! -s "${WORK}/port" ]; do sleep 0.1; done
    local url i
    url="http://127.0.0.1:$(cat "${WORK}/port")"
    for i in $(seq "$2"); do
        CI_COMMIT_TAG=v9.9.9 CI_COMMIT_SHA="${SHA}" GITHUB_TOKEN=fake-token-value \
            GITHUB_REPO="${3:-o/r}" GITLAB_MIRROR_TOKEN="${4:-}" CI_API_V4_URL="${url}/api/v4" CI_PROJECT_ID=7 GITHUB_API="${url}" GITHUB_UPLOADS="${url}" \
            MIRROR_WAIT_TIMEOUT=5 MIRROR_POLL_INTERVAL=1 \
            bash scripts/release-github.sh > "${WORK}/out" 2>&1 || { cat "${WORK}/out"; echo "FAIL: run ${i}"; exit 1; }
    done
    kill "${SRV_PID}"; SRV_PID=""
    if grep -q 'fake-token-value\|fake-mirror-token' "${WORK}/out"; then echo "FAIL: token in log"; exit 1; fi
    grep -qx "Bearer fake-token-value" "${WORK}/tokens" || { echo "FAIL: token header missing"; exit 1; }
}
expect() { grep -qx "$1" "${WORK}/state" || { echo "FAIL: expected '$1'"; cat "${WORK}/state"; exit 1; }; }

run_case 0 2   # fresh create, then re-run: update + replace all 3 assets
expect create; expect patch; expect delete
expect "upload ffmpeg-7.1.tar.gz"; expect "upload ffmpeg-9.0.tar.gz"; expect "upload CHANGELOG.md"
[ "$(grep -c '^delete$' "${WORK}/state")" = 3 ] || { echo "FAIL: expected 3 deletes"; exit 1; }
# CHANGELOG.md asset must be NOTES.md (same as the GitLab/S3 asset), not the repo file.
cmp "${WORK}/repo/NOTES.md" "${WORK}/state.asset" || { echo "FAIL: CHANGELOG.md asset != NOTES.md"; exit 1; }
run_case 1 1   # release made by hand: updated, never created
expect patch
if grep -qx create "${WORK}/state"; then echo "FAIL: created over existing release"; exit 1; fi
echo "release-github fake API: OK"

run_case 0 1 https://github.com/o/r.git fake-mirror-token   # URL repo + mirror trigger
expect create; expect mirror-sync
run_case 0 1 git@github.com:o/r.git                         # scp-style repo, no token: no trigger
expect create
if grep -qx mirror-sync "${WORK}/state"; then echo "FAIL: mirror triggered without token"; exit 1; fi
grep -q 'GITLAB_MIRROR_TOKEN unset' "${WORK}/out" || { echo "FAIL: missing skip note"; exit 1; }
echo "release-github URL repo + mirror trigger: OK"
