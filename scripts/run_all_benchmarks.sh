#!/bin/bash
# Run all baseline benchmarks and save results

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOONCAKE_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$MOONCAKE_ROOT/build}"
RESULTS_DIR="${RESULTS_DIR:-$MOONCAKE_ROOT/benchmark_results/baseline}"

mkdir -p "$RESULTS_DIR"

echo "=== Running All Baseline Benchmarks ==="
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Check if benchmarks are built
BENCH_DIR="$BUILD_DIR/mooncake-transfer-engine/benchmark"
if [ ! -f "$BENCH_DIR/shm/shm_allocation_bench" ]; then
    echo "Error: Benchmarks not built. Run ./scripts/build_benchmarks.sh first"
    exit 1
fi

# Run SHM benchmarks
echo "--- SHM Allocation Benchmark ---"
"$BENCH_DIR/shm/shm_allocation_bench" \
    --num_iterations=1000 \
    --min_size_kb=4 \
    --max_size_kb=1024 \
    --cleanup=true \
    | tee "$RESULTS_DIR/shm_allocation.txt"

echo ""
echo "--- SHM Address Lookup Benchmark ---"
"$BENCH_DIR/shm/shm_address_lookup_bench" \
    --num_segments=100 \
    --num_lookups=10000 \
    --segment_size_mb=64 \
    | tee "$RESULTS_DIR/shm_address_lookup.txt"

echo ""
echo "--- SHM Transfer Benchmark ---"
"$BENCH_DIR/shm/shm_transfer_bench" \
    --transfer_size_kb=4 \
    --max_transfer_size_mb=64 \
    --num_transfers=1000 \
    --use_memcpy=true \
    | tee "$RESULTS_DIR/shm_transfer.txt"

echo ""
echo "--- Control Plane RPC Benchmark ---"
"$BENCH_DIR/control_plane/control_plane_rpc_bench" \
    --num_iterations=1000 \
    --warmup_iterations=100 \
    --min_data_size_kb=4 \
    --max_data_size_mb=16 \
    | tee "$RESULTS_DIR/control_plane_rpc.txt"

echo ""
echo "=== All Benchmarks Complete ==="
echo ""
echo "Results saved in: $RESULTS_DIR"
echo ""
echo "View results:"
echo "  cat $RESULTS_DIR/*.txt"
echo ""
echo "Key findings:"
echo "  - SHM allocation: Check 'Total (all 3 syscalls)' mean time"
echo "  - Address lookup: Compare linear scan vs arithmetic translation"
echo "  - sendData/recvData: Note latency for large data sizes (CRITICAL)"
echo "  - JSON overhead: Check serialization times"
