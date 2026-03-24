#!/bin/bash
# Silex build script: builds all image variants
# Usage: ./scripts/build.sh [--only slim|dev|runtime|cross]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${SILEX_VERSION:-latest}"
ONLY=""

# Colours
BLUE='\033[0;34m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) ONLY="$2"; shift 2 ;;
        *) echo -e "${RED}Unknown argument: $1${NC}"; exit 1 ;;
    esac
done

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Silex Build Script${NC}"
echo -e "${BLUE}=====================================${NC}"
echo ""

build_variant() {
    local name="$1" dockerfile="$2"
    if [ ! -f "$ROOT/dockerfiles/${dockerfile}" ]; then
        echo -e "${YELLOW}Skipping silex:${name} (${dockerfile} not found)${NC}"
        return 0
    fi
    echo -e "${YELLOW}Building silex:${name}...${NC}"
    docker build -f "$ROOT/dockerfiles/${dockerfile}" \
        -t "silex:${name}" -t "silex:${name}-${VERSION}" "$ROOT"
    echo -e "${GREEN}✓ silex:${name} ($(docker images silex:${name} --format '{{.Size}}'))${NC}"
    echo ""
}

if [[ -z "$ONLY" || "$ONLY" == "slim" ]];    then build_variant slim    Dockerfile.slim;    fi
if [[ -z "$ONLY" || "$ONLY" == "dev" ]];     then build_variant dev     Dockerfile.dev;     fi
if [[ -z "$ONLY" || "$ONLY" == "runtime" ]]; then build_variant runtime Dockerfile.runtime; fi
if [[ -z "$ONLY" || "$ONLY" == "cross" ]];   then build_variant cross   Dockerfile.cross;   fi

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Build Summary${NC}"
echo -e "${BLUE}=====================================${NC}"
docker images silex

echo ""
echo -e "${GREEN}Done.${NC}"
