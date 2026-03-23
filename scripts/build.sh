#!/bin/bash
# Silex build script: builds all image variants
# MVP: Only slim variant is implemented

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colours
BLUE='\033[0;34m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Silex Build Script${NC}"
echo -e "${BLUE}=====================================${NC}"
echo ""

# Version (will be configurable later)
VERSION="${SILEX_VERSION:-latest}"

# Build slim variant
echo -e "${YELLOW}Building silex:slim...${NC}"
docker build \
    -f "$PROJECT_ROOT/dockerfiles/Dockerfile.slim" \
    -t "silex:slim" \
    -t "silex:slim-$VERSION" \
    "$PROJECT_ROOT"

echo -e "${GREEN}✓ silex:slim built successfully${NC}"
echo ""

# Future: Build other variants (full, dev, cross, runtime)
# echo -e "${YELLOW}Building silex:full...${NC}"
# docker build -f "$PROJECT_ROOT/dockerfiles/Dockerfile.full" -t "silex:full" "$PROJECT_ROOT"
# ...

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Build Summary${NC}"
echo -e "${BLUE}=====================================${NC}"
docker images silex

echo ""
echo -e "${GREEN}All images built successfully!${NC}"
echo ""
echo "Run the benchmark to test performance:"
echo "  cd benchmarks && ./benchmark.sh"
