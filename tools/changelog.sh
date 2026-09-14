#!/usr/bin/env bash
# Render a Markdown changelog from the Conventional Commits between the previous tag and TAG.
#
#   tools/changelog.sh            # HEAD, against the newest tag that is an ancestor
#   tools/changelog.sh v1.0.0     # that tag, against the one before it
#
# Commit links need the owner/repo, taken from GITHUB_REPOSITORY or the origin remote; without
# either, entries degrade to bare short hashes instead of failing.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

ref="${1:-HEAD}"
git rev-parse --verify --quiet "$ref^{commit}" >/dev/null || {
    echo "unknown ref: $ref" >&2
    exit 1
}

prev="$(git describe --tags --abbrev=0 --match 'v*' "$ref^" 2>/dev/null || true)"
range="${prev:+$prev..}$ref"

slug="${GITHUB_REPOSITORY:-}"
if [ -z "$slug" ]; then
    origin="$(git remote get-url origin 2>/dev/null || true)"
    # Bash uses POSIX ERE, where "+?" is not a lazy quantifier: a pattern like "[^/]+?(\.git)?"
    # stays greedy and keeps the ".git", producing links to github.com/owner/repo.git/commit/...
    if [[ "$origin" =~ github\.com[:/]+(.+)$ ]]; then
        slug="${BASH_REMATCH[1]%/}"
        slug="${slug%.git}"
    fi
fi

# Section keys in output order. "!" collects the breaking changes, "?" the commits whose subject
# does not parse as Conventional Commits — dropping those would silently hide real work.
order=('!' feat fix perf refactor docs test build ci chore revert style '?')
declare -A title=(
    ['!']='Breaking changes' [feat]='Features' [fix]='Fixes' [perf]='Performance'
    [refactor]='Refactoring' [docs]='Documentation' [test]='Tests' [build]='Build'
    [ci]='CI' [chore]='Chores' [revert]='Reverts' [style]='Style' ['?']='Other'
)
declare -A body=()

# %x1e separates commits, %x1f the fields, so a subject or body containing either is the only way
# to confuse this — and neither is typeable.
while IFS= read -r -d $'\x1e' commit; do
    # git puts a newline after every record; the body is multi-line, so split on the field
    # separator by hand rather than with `read`, which would stop at the first line.
    commit="${commit#$'\n'}"
    [ -n "$commit" ] || continue
    sha="${commit%%$'\x1f'*}"
    rest="${commit#*$'\x1f'}"
    subject="${rest%%$'\x1f'*}"
    notes="${rest#*$'\x1f'}"

    key='?' scope='' text="$subject"
    if [[ "$subject" =~ ^([a-zA-Z]+)(\(([^\)]*)\))?(!)?:[[:space:]]*(.+)$ ]]; then
        key="${BASH_REMATCH[1],,}"
        scope="${BASH_REMATCH[3]}"
        text="${BASH_REMATCH[5]}"
        [ -n "${BASH_REMATCH[4]}" ] && key='!'
        [ -n "${title[$key]+x}" ] || key='?'
    fi
    [[ "$notes" == *'BREAKING CHANGE:'* || "$notes" == *'BREAKING-CHANGE:'* ]] && key='!'

    short="${sha:0:7}"
    link="\`$short\`"
    [ -n "$slug" ] && link="[\`$short\`](https://github.com/$slug/commit/$sha)"

    body[$key]+="- ${scope:+**$scope**: }${text} ($link)"$'\n'
done < <(git log --no-merges --format='%H%x1f%s%x1f%b%x1e' "$range")

for key in "${order[@]}"; do
    [ -n "${body[$key]:-}" ] || continue
    printf '### %s\n\n%s\n' "${title[$key]}" "${body[$key]}"
done

if [ -z "${body[*]:-}" ]; then
    echo "_No commits in \`$range\`._"
    echo
fi

if [ -n "$slug" ]; then
    if [ -n "$prev" ]; then
        echo "**Full changelog**: https://github.com/$slug/compare/$prev...$ref"
    else
        echo "**Full changelog**: https://github.com/$slug/commits/$ref"
    fi
fi
