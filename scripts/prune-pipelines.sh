#!/usr/bin/env bash
# Delete failed, skipped and passed-with-warnings pipelines of this GitLab
# project. Deletion is permanent (jobs, logs and artifacts go with it).
#
# The project is resolved by glab from the repo's git remote (:id), so the
# script must run inside a checkout; it works from any working directory.
#
# Usage:
#   scripts/prune-pipelines.sh [--status LIST]
#
# Options:
#   --status LIST  Comma-separated categories: failed,skipped,warnings
#                  (default: all three).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

categories="failed,skipped,warnings"
while [ $# -gt 0 ]; do
    case "$1" in
        --status) categories="${2:?--status needs a value}"; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v glab >/dev/null || { echo "glab not found" >&2; exit 1; }
command -v jq >/dev/null || { echo "jq not found" >&2; exit 1; }
glab auth status >/dev/null 2>&1 || { echo "glab not authenticated: run glab auth login" >&2; exit 1; }

# list_ids STATUS: ids of all pipelines with that API status.
list_ids() {
    glab api --paginate "projects/:id/pipelines?status=$1&per_page=100" | jq -r '.[].id'
}

# warning_ids: ids of success pipelines whose detailed status group is
# success-with-warnings (allowed-to-fail jobs failed).
warning_ids() {
    local path
    path=$(glab api "projects/:id" | jq -r '.path_with_namespace')
    # shellcheck disable=SC2016 # GraphQL variables, not shell
    glab api graphql --paginate -F path="$path" -f query='
        query($path: ID!, $endCursor: String) {
          project(fullPath: $path) {
            pipelines(status: SUCCESS, first: 100, after: $endCursor) {
              nodes { id detailedStatus { group } }
              pageInfo { hasNextPage endCursor }
            }
          }
        }' |
        jq -r '.data.project.pipelines.nodes[]
               | select(.detailedStatus.group == "success-with-warnings")
               | .id | sub(".*/"; "")'
}

ids=()
IFS=',' read -r -a cats <<< "$categories"
for cat in "${cats[@]}"; do
    case "$cat" in
        failed|skipped)
            mapfile -t found < <(list_ids "$cat")
            ;;
        warnings)
            # The REST list has no detailed_status (one GET per pipeline is
            # slow); GraphQL returns it per page of 100.
            mapfile -t found < <(warning_ids)
            ;;
        *) echo "unknown category: $cat" >&2; exit 2 ;;
    esac
    echo "$cat: ${#found[@]}"
    ids+=("${found[@]}")
done

if [ ${#ids[@]} -eq 0 ]; then
    echo "nothing to delete"
    exit 0
fi

deleted=0
errors=0
for id in "${ids[@]}"; do
    if glab api --method DELETE "projects/:id/pipelines/$id" >/dev/null 2>&1; then
        deleted=$((deleted + 1))
    else
        echo "delete failed: $id" >&2
        errors=$((errors + 1))
    fi
done
echo "deleted: $deleted, errors: $errors"
[ "$errors" -eq 0 ]
