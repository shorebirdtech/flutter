#!/usr/bin/env bash
# Finds an earlier pass of an engine CI job on the same inputs, so a run
# whose inputs already passed doesn't build and test them again.
#
#   shorebird/ci/engine_pass.sh <job>
#
# <job> is the workflow's name less its prefix: engine_tests,
# ios_engine_build or android_engine_build (shorebird_<job>.yml). Those jobs
# read DEPS (which pins the Dart SDK, the toolchains and every third_party
# dep), engine/, their workflow and this script, and nothing else in the
# repo, so the git hashes of those four are the key. The common hit: a PR's
# run tests its merge with shorebird/dev, and when nothing that changes
# DEPS or engine/ lands before it is squashed, the push of that squash has
# the same inputs and needn't run again.
#
# Once a job passes, its workflow's record job uploads an artifact named
# shorebird-pass-<job>-<key> holding the run's URL. This looks one up and writes, to
# $GITHUB_OUTPUT:
#   artifact  the name the job uploads when it passes
#   run       the URL of a run that passed on these inputs, or empty
#
# Only runs of this repo's own branches count: a fork PR's run can upload
# any artifact it likes, since it runs the PR's copy of the workflow.
# Lookup errors print a warning and leave `run` empty, so the job runs.
set -euo pipefail

job="$1"
workflow=".github/workflows/shorebird_$job.yml"
# ls-tree lists a missing path as nothing, so a wrong <job> would still
# make a key.
git cat-file -e "HEAD:$workflow" || { echo "::error::No $workflow"; exit 1; }
key=$(git ls-tree HEAD DEPS engine "$workflow" shorebird/ci/engine_pass.sh | sha256sum | cut -c1-40)
artifact="shorebird-pass-$job-$key"
echo "artifact=$artifact" >> "$GITHUB_OUTPUT"

# A re-run attempt runs the job for real: someone re-ran it on purpose
# (a new runner image or Xcode, which the key doesn't cover).
if [[ "${GITHUB_RUN_ATTEMPT:-1}" -gt 1 ]]; then
  echo "run=" >> "$GITHUB_OUTPUT"
  echo "Re-run attempt $GITHUB_RUN_ATTEMPT; running the job."
  exit 0
fi

if ! id=$(gh api "repos/$GITHUB_REPOSITORY/actions/artifacts?name=$artifact&per_page=100" \
    --jq "[.artifacts[] | select((.expired | not) and .workflow_run.head_repository_id == $GITHUB_REPOSITORY_ID)][0].workflow_run.id // empty"); then
  echo "::warning::Couldn't look up $artifact; running the job."
  id=""
fi

if [[ -n "$id" ]]; then
  url="$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$id"
  echo "run=$url" >> "$GITHUB_OUTPUT"
  echo "Passed on these inputs (key $key) in $url; not running it again." | tee -a "$GITHUB_STEP_SUMMARY"
else
  echo "run=" >> "$GITHUB_OUTPUT"
  echo "No pass on these inputs (key $key); running the job."
fi
