#!/bin/bash
# ============================================================
#  build.sh – Run this on your Mac to compile via Docker
#  Requirements: Docker Desktop must be running
# ============================================================

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="miyoo-audiobook-toolchain"

echo "Building MiyooAudiobook..."
echo "Project: $PROJECT_DIR"

# Build local Docker image if not present
if ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "Building Docker toolchain image (first time, takes 2-3 minutes)..."
    docker build -t "$IMAGE" "$PROJECT_DIR"
fi

# Compile inside Docker
docker run --rm \
    -v "$PROJECT_DIR":/work \
    -w /work \
    "$IMAGE" \
    make

echo ""
echo "Done! Binary: $PROJECT_DIR/audiobook-player"
echo "Now run:  make package  (or ./scripts/deploy.sh)"
