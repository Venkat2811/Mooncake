# SHM Micro-Benchmarks

This directory contains micro-benchmarks for measuring the performance of shared memory operations in Mooncake's transfer engine. These benchmarks establish baseline performance metrics before Flow-IPC integration.

## Benchmarks

### 1. shm_allocation_bench

Measures the performance of SHM allocation operations (shm_open, ftruncate, mmap).

**Usage:**
```bash
./shm_allocation_bench \
    --num_iterations=1000 \
    --min_size_kb=4 \
    --max_size_kb=1024 \
    --cleanup=true
```

**Flags:**
- `--num_iterations`: Number of allocation iterations (default: 1000)
- `--min_size_kb`: Minimum allocation size in KB (default: 4)
- `--max_size_kb`: Maximum allocation size in KB (default: 1024)
- `--measure_mmap_only`: Only measure mmap, not shm_open (default: false)
- `--cleanup`: Clean up SHM segments after test (default: true)

**What it measures:**
- shm_open() latency
- ftruncate() latency
- mmap() latency
- Total allocation time (all 3 syscalls)
- Cleanup time (munmap + close + unlink)
- Allocation throughput (allocations/sec)

**Baseline expectations (current Mooncake):**
- Total allocation time: ~10-50 μs per allocation (3 syscalls)
- Dominated by syscall overhead

**Flow-IPC target:**
- Arena allocation: ~100-500 ns (100x improvement)
- Pre-allocated pool eliminates per-allocation syscalls

---

### 2. shm_address_lookup_bench

Compares different address translation strategies for SHM segments.

**Usage:**
```bash
./shm_address_lookup_bench \
    --num_segments=100 \
    --num_lookups=10000 \
    --segment_size_mb=64
```

**Flags:**
- `--num_segments`: Number of SHM segments to simulate (default: 100)
- `--num_lookups`: Number of lookup operations (default: 10000)
- `--segment_size_mb`: Size of each segment in MB (default: 64)

**What it measures:**
- **Linear scan (O(n))**: Current Mooncake implementation in relocateSharedMemoryAddress()
- **Map lower_bound (O(log n))**: Optimized tree-based lookup
- **Arithmetic translation (O(1))**: Flow-IPC style offset calculation

**Baseline expectations (current Mooncake):**
- Linear scan: ~100-1000 ns per lookup for 100 segments
- Performance degrades linearly with segment count

**Flow-IPC target:**
- Arithmetic translation: ~5-10 ns per lookup (O(1))
- Performance independent of segment count

---

### 3. shm_transfer_bench

Measures memcpy throughput within shared memory regions.

**Usage:**
```bash
./shm_transfer_bench \
    --transfer_size_kb=4 \
    --max_transfer_size_mb=64 \
    --num_transfers=1000 \
    --use_memcpy=true \
    --verify_data=false
```

**Flags:**
- `--transfer_size_kb`: Transfer size in KB (default: 4)
- `--max_transfer_size_mb`: Maximum transfer size for sweep in MB (default: 64)
- `--num_transfers`: Number of transfers per size (default: 1000)
- `--use_memcpy`: Use memcpy for transfers (default: true)
- `--verify_data`: Verify data after transfer (default: false)

**What it measures:**
- memcpy performance in SHM (DRAM-to-DRAM transfers)
- Bandwidth at different transfer sizes
- Average transfer latency

**Baseline expectations:**
- Small transfers (4KB): ~5-10 GB/s
- Large transfers (64MB): ~30-60 GB/s
- Limited by memory bandwidth, not SHM mechanism

**Note:** This establishes that the transfer path itself (memcpy) is not the bottleneck. The improvements come from allocation and address translation optimizations.

---

## Running All Benchmarks

```bash
# Build benchmarks
cd /root/Documents/kvcache-opti/Mooncake/mooncake-transfer-engine
mkdir build && cd build
cmake .. -DBUILD_UNIT_TESTS=ON
make shm_allocation_bench shm_address_lookup_bench shm_transfer_bench

# Run all benchmarks and save results
./benchmark/shm/shm_allocation_bench > baseline_allocation.txt
./benchmark/shm/shm_address_lookup_bench > baseline_lookup.txt
./benchmark/shm/shm_transfer_bench > baseline_transfer.txt

# View results
cat baseline_*.txt
```

## Interpreting Results

### Key Metrics to Track

1. **Allocation Latency** (from shm_allocation_bench):
   - Current: ~10-50 μs
   - Target with Flow-IPC: ~100-500 ns
   - Expected improvement: 100x

2. **Address Lookup Time** (from shm_address_lookup_bench):
   - Current: O(n) linear scan
   - Target with Flow-IPC: O(1) arithmetic
   - Expected improvement: 10-100x depending on segment count

3. **Transfer Bandwidth** (from shm_transfer_bench):
   - Current: Memory bandwidth limited
   - Target with Flow-IPC: Same (no change expected)
   - This confirms memcpy is not the bottleneck

### Comparison Script

After implementing Flow-IPC improvements, compare results:

```bash
# Save baseline
mkdir -p baseline_results
mv baseline_*.txt baseline_results/

# Run again after Flow-IPC integration
./benchmark/shm/shm_allocation_bench > flow_ipc_allocation.txt
./benchmark/shm/shm_address_lookup_bench > flow_ipc_lookup.txt
./benchmark/shm/shm_transfer_bench > flow_ipc_transfer.txt

# Compare
diff -u baseline_results/baseline_allocation.txt flow_ipc_allocation.txt
```

## Expected Improvements Summary

| Operation | Baseline | With Flow-IPC | Speedup |
|-----------|----------|---------------|---------|
| SHM Allocation | ~10-50 μs | ~100-500 ns | 100x |
| Address Lookup (100 segments) | ~500 ns | ~5-10 ns | 50-100x |
| Memory Transfer | 30-60 GB/s | 30-60 GB/s | 1x (no change) |

## Integration with CI

These benchmarks can be integrated into CI to detect performance regressions:

```yaml
# In .github/workflows/ci.yml
- name: Run SHM benchmarks
  run: |
    ./benchmark/shm/shm_allocation_bench --num_iterations=100
    ./benchmark/shm/shm_address_lookup_bench --num_lookups=1000
    ./benchmark/shm/shm_transfer_bench --num_transfers=100
```

## Troubleshooting

### Permission Issues

If you see "Permission denied" errors:
```bash
sudo chmod 777 /dev/shm
# Or run with sufficient permissions
```

### Memory Limits

If benchmarks fail with "No space left on device":
```bash
# Check SHM usage
df -h /dev/shm

# Clean up old SHM segments
ls -la /dev/shm
rm /dev/shm/mooncake_*
```

### Inconsistent Results

- Run benchmarks multiple times and average results
- Disable CPU frequency scaling for more consistent results:
  ```bash
  sudo cpupower frequency-set --governor performance
  ```
- Isolate CPU cores if needed:
  ```bash
  taskset -c 0-3 ./shm_allocation_bench
  ```
