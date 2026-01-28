#!/bin/bash
# Build script for Flow-IPC integration benchmarks

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOONCAKE_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$MOONCAKE_ROOT/build}"

echo "=== Building Mooncake with Flow-IPC Benchmarks ==="
echo "Mooncake root: $MOONCAKE_ROOT"
echo "Build directory: $BUILD_DIR"
echo ""

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "--- Running CMake ---"
cmake "$MOONCAKE_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_UNIT_TESTS=ON \
    -DUSE_CUDA=OFF \
    -DUSE_TCP=ON

echo ""
echo "--- Building Benchmarks ---"

# Build SHM benchmarks
echo "Building SHM benchmarks..."
make shm_allocation_bench shm_address_lookup_bench shm_transfer_bench -j$(nproc)

# Build control plane benchmarks
echo "Building control plane benchmarks..."
make control_plane_rpc_bench -j$(nproc)

echo ""
echo "=== Build Complete ==="
echo ""
echo "Benchmark locations:"
echo "  SHM benchmarks:"
echo "    - $BUILD_DIR/mooncake-transfer-engine/benchmark/shm/shm_allocation_bench"
echo "    - $BUILD_DIR/mooncake-transfer-engine/benchmark/shm/shm_address_lookup_bench"
echo "    - $BUILD_DIR/mooncake-transfer-engine/benchmark/shm/shm_transfer_bench"
echo ""
echo "  Control plane benchmarks:"
echo "    - $BUILD_DIR/mooncake-transfer-engine/benchmark/control_plane/control_plane_rpc_bench"
echo ""
echo "To run all benchmarks:"
echo "  ./scripts/run_all_benchmarks.sh"
