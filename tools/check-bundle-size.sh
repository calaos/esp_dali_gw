#!/usr/bin/env bash
# Enforce the web bundle budget (SPEC 10): 100 KB gzipped, total, all assets.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUDGET_BYTES=$((100 * 1024))
[ -d web/dist ] || { echo "web/dist missing; run npm run build in web/"; exit 1; }

total=0
while IFS= read -r f; do
    sz=$(gzip -9 -c "$f" | wc -c)
    printf '  %8d  %s\n' "$sz" "${f#web/dist/}"
    total=$((total + sz))
done < <(find web/dist -type f | sort)

echo "  --------"
printf '  %8d  TOTAL gzipped (budget %d)\n' "$total" "$BUDGET_BYTES"

if [ "$total" -gt "$BUDGET_BYTES" ]; then
    echo "FAIL: bundle is over budget by $((total - BUDGET_BYTES)) bytes" >&2
    exit 1
fi
printf 'OK: %d%% of budget used\n' $((total * 100 / BUDGET_BYTES))
