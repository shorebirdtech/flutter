#!/bin/bash -e

# Usage:
# ./linux_build_and_upload.sh flutter_root engine_hash
#
# This is the main entrypoint for building and uploading Linux engine artifacts.
# It is called from the _build_engine repository's CI scripts.

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 flutter_root engine_hash"
    exit 1
fi

FLUTTER_ROOT=$1
ENGINE_HASH=$2
ENGINE_ROOT=$FLUTTER_ROOT/engine

# Get the absolute path to the directory of this script.
SCRIPT_DIR=$(cd $(dirname $0) && pwd)

echo "Building engine at $ENGINE_ROOT and uploading to gs://download.shorebird.dev"

cd $SCRIPT_DIR

# Run the setup script.
./internal/linux_setup.sh

# Then run the build.
./internal/linux_build.sh $ENGINE_ROOT

# Copy Shorebird engine artifacts to Google Cloud Storage.
./internal/linux_upload.sh $ENGINE_ROOT $ENGINE_HASH
