#!/bin/bash
# Silex benchmark script: compare build times vs ubuntu:24.04
# Runs the same C++ project build in both environments and reports timing

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/projects/cpp-json-parser"
RESULTS_DIR="$SCRIPT_DIR/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Colours for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Silex Build Performance Benchmark${NC}"
echo -e "${BLUE}=====================================${NC}"
echo ""

# Create results directory
mkdir -p "$RESULTS_DIR"

# ============================================================================
# Build Silex image
# ============================================================================
echo -e "${YELLOW}Building Silex image...${NC}"
cd "$SCRIPT_DIR/.."
docker build -f dockerfiles/Dockerfile.slim -t silex:slim . || {
    echo -e "${RED}Failed to build Silex image${NC}"
    exit 1
}
echo -e "${GREEN}✓ Silex image built${NC}"
echo ""

# ============================================================================
# Create Ubuntu comparison Dockerfile
# ============================================================================
echo -e "${YELLOW}Creating Ubuntu comparison image...${NC}"
cat > /tmp/Dockerfile.ubuntu-build <<'EOF'
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Use bash as shell
SHELL ["/bin/bash", "-c"]
EOF

docker build -f /tmp/Dockerfile.ubuntu-build -t ubuntu-build:test . || {
    echo -e "${RED}Failed to build Ubuntu comparison image${NC}"
    exit 1
}
echo -e "${GREEN}✓ Ubuntu comparison image built${NC}"
echo ""

# ============================================================================
# Benchmark function
# ============================================================================
run_benchmark() {
    local image="$1"
    local name="$2"
    local runs=3

    echo -e "${YELLOW}Benchmarking ${name}...${NC}"

    local total_time=0
    local best_time=999999
    local worst_time=0

    for i in $(seq 1 $runs); do
        echo -n "  Run $i/$runs: "

        # Run build and time it
        local start=$(date +%s%N)

        docker run --rm \
            -v "$PROJECT_DIR:/workspace" \
            -w /workspace \
            "$image" \
            bash -c "
                rm -rf build && \
                cmake -B build -DCMAKE_BUILD_TYPE=Release && \
                cmake --build build
            " > /dev/null 2>&1

        local end=$(date +%s%N)
        local duration=$(( (end - start) / 1000000 )) # Convert to milliseconds
        local duration_sec=$(echo "scale=2; $duration / 1000" | bc)

        echo "${duration_sec}s"

        total_time=$(( total_time + duration ))

        if (( duration < best_time )); then
            best_time=$duration
        fi

        if (( duration > worst_time )); then
            worst_time=$duration
        fi
    done

    local avg_time=$(( total_time / runs ))
    local avg_sec=$(echo "scale=2; $avg_time / 1000" | bc)
    local best_sec=$(echo "scale=2; $best_time / 1000" | bc)
    local worst_sec=$(echo "scale=2; $worst_time / 1000" | bc)

    echo -e "${GREEN}  Average: ${avg_sec}s (best: ${best_sec}s, worst: ${worst_sec}s)${NC}"
    echo ""

    # Return average time in milliseconds
    echo "$avg_time"
}

# ============================================================================
# Run benchmarks
# ============================================================================
silex_time=$(run_benchmark "silex:slim" "Silex")
ubuntu_time=$(run_benchmark "ubuntu-build:test" "Ubuntu 24.04")

# ============================================================================
# Calculate speedup
# ============================================================================
speedup=$(echo "scale=2; $ubuntu_time / $silex_time" | bc)
silex_sec=$(echo "scale=2; $silex_time / 1000" | bc)
ubuntu_sec=$(echo "scale=2; $ubuntu_time / 1000" | bc)
time_saved=$(echo "scale=2; ($ubuntu_time - $silex_time) / 1000" | bc)

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Results${NC}"
echo -e "${BLUE}=====================================${NC}"
echo -e "Silex:      ${GREEN}${silex_sec}s${NC}"
echo -e "Ubuntu:     ${ubuntu_sec}s"
echo -e "Speedup:    ${GREEN}${speedup}x${NC}"
echo -e "Time saved: ${GREEN}${time_saved}s${NC}"
echo ""

if (( $(echo "$speedup >= 2.0" | bc -l) )); then
    echo -e "${GREEN}✓ SUCCESS: Silex is ${speedup}x faster than Ubuntu!${NC}"
elif (( $(echo "$speedup >= 1.5" | bc -l) )); then
    echo -e "${YELLOW}⚠ MODERATE: Silex is ${speedup}x faster (target: 2-3x)${NC}"
else
    echo -e "${RED}✗ FAIL: Silex speedup is only ${speedup}x (target: 2-3x)${NC}"
fi
echo ""

# ============================================================================
# Save results to file
# ============================================================================
RESULTS_FILE="$RESULTS_DIR/benchmark_$TIMESTAMP.txt"
cat > "$RESULTS_FILE" <<EOF
Silex Build Performance Benchmark
Timestamp: $(date)
Project: C++ JSON Parser

Results:
--------
Silex (avg):    ${silex_sec}s
Ubuntu (avg):   ${ubuntu_sec}s
Speedup:        ${speedup}x
Time saved:     ${time_saved}s

Environment:
------------
Silex image:    silex:slim
Ubuntu image:   ubuntu:24.04 + build-essential + cmake + ninja-build
Test project:   benchmarks/projects/cpp-json-parser
Build command:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
Runs per test:  3

System:
-------
$(uname -a)
Docker version: $(docker --version)
EOF

echo -e "Results saved to: ${RESULTS_FILE}"

# ============================================================================
# Image size comparison
# ============================================================================
echo ""
echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Image Size Comparison${NC}"
echo -e "${BLUE}=====================================${NC}"

silex_size=$(docker images silex:slim --format "{{.Size}}")
ubuntu_size=$(docker images ubuntu-build:test --format "{{.Size}}")

echo -e "Silex:  ${silex_size}"
echo -e "Ubuntu: ${ubuntu_size}"
echo ""

# ============================================================================
# Cleanup
# ============================================================================
echo -e "${YELLOW}Cleanup${NC}"
echo -n "Remove test images? [y/N] "
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    docker rmi ubuntu-build:test 2>/dev/null || true
    echo -e "${GREEN}✓ Cleaned up test images${NC}"
fi

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
