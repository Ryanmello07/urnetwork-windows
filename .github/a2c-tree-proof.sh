#!/bin/bash
# The job's product tree is the named commit's (the A2c review's A-4): fetches
# the commit that .github/a2c-tree-of.txt names, from this repository, and
# prints `git diff --stat <commit> HEAD -- . ':!.github'`, which must be empty.
# Run from the root of the Windows checkout. Rig only: never merged.
set -euo pipefail
sha=$(tr -d '\r\n ' < .github/a2c-tree-of.txt)
if ! printf '%s' "$sha" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "tree proof: FAIL, .github/a2c-tree-of.txt holds '$sha', not a full commit id"
  exit 1
fi
git fetch --no-tags --depth=1 origin "$sha"
echo "tree proof: HEAD $(git rev-parse HEAD) on $GITHUB_REF_NAME; named commit $sha"
echo "tree proof: git diff --stat $sha HEAD -- . ':!.github'"
stat=$(git diff --stat "$sha" HEAD -- . ':!.github')
if [ -n "$stat" ]; then
  printf '%s\n' "$stat"
  echo "tree proof: FAIL, outside .github the tree is not $sha's"
  exit 1
fi
echo "tree proof: (empty) OK, outside .github the tree is $sha's"
