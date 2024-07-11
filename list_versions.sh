#!/bin/bash

# Get all version branches
BRANCHES=$(git branch -r | grep origin/flutter_release)

echo "Flutter versions:"
for branch in $BRANCHES; do
  REV=$(git rev-parse $branch)
  echo "$branch $REV"
done
