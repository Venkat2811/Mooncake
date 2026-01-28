# Control Plane RPC Benchmarks

This directory contains benchmarks for measuring control plane RPC performance in Mooncake's transfer engine. These benchmarks establish baseline metrics for RPC latency and serialization overhead before Flow-IPC integration.

## Benchmark

### control_plane_rpc_bench

Measures the performance of control plane RPC operations and identifies optimization opportunities.

**Usage:**
```bash
./control_plane_rpc_bench \
    --server_addr=127.0.0.1:9000 \
    --num_iterations=1000 \
    --warmup_iterations=100 \
    --min_data_size_kb=4 \
    --max_data_size_mb=16
```

**Flags:**
- `--server_addr`: RPC server address (default: "127.0.0.1:9000")
- `--num_iterations`: Number of RPC iterations (default: 1000)
- `--warmup_iterations`: Number of warmup iterations (default: 100)
- `--min_data_size_kb`: Minimum data transfer size in KB (default: 4)
- `--max_data_size_mb`: Maximum data transfer size in MB (default: 16)
- `--run_server`: Run as server instead of client (default: false)

## What It Measures

### 1. getSegmentDesc() RPC
Basic metadata query with minimal payload.

**Baseline expectations:**
- Mean latency: ~100-500 μs
- Dominated by network round-trip on loopback
- JSON parsing overhead: minimal (small message)

### 2. bootstrap() RPC
RDMA connection setup with structured data.

**Baseline expectations:**
- Mean latency: ~200-800 μs
- JSON serialization: ~5-20 μs
- Includes struct with vectors and strings

### 3. notify() RPC
Simple notification messages.

**Baseline expectations:**
- Mean latency: ~100-400 μs
- Small payload overhead

### 4. sendData() / recvData() RPC (HOT PATH!)
**Critical**: These operations **copy data through RPC** instead of using zero-copy SHM.

**Current implementation (control_plane.cpp:48-70):**
```cpp
Status ControlClient::sendData(..., void* local_mem_addr, size_t length) {
    std::string request;
    request.resize(sizeof(XferDataDesc) + length);
    memcpy(&request[0], &desc, sizeof(desc));
    Platform::getLoader().copy(&request[sizeof(desc)], local_mem_addr, length);
    return tl_rpc_agent.call(server_addr, SendData, request, response);
}
```

**Performance characteristics:**
- 4 KB: ~200-600 μs (dominated by RPC overhead)
- 1 MB: ~2-10 ms (memcpy + network transfer)
- 4 MB: ~10-50 ms (bandwidth limited)

**Flow-IPC opportunity:**
- Eliminate memcpy into RPC buffer
- Use SHM-backed structured channels
- Only send offset/handle over socket
- Expected: ~100-200 μs regardless of data size

### 5. JSON Serialization Overhead
Measures pure serialization/deserialization cost.

**Baseline expectations:**
- BootstrapDesc: ~5-20 μs
- Notification: ~2-10 μs
- Large JSON (1KB): ~20-50 μs

**Flow-IPC opportunity:**
- Replace with Cap'n Proto (zero-copy)
- No serialization step for SHM-backed messages
- Expected: <1 μs (accessor overhead only)

## Running the Benchmark

```bash
# Build
cd /root/Documents/kvcache-opti/Mooncake/mooncake-transfer-engine/build
cmake .. -DBUILD_UNIT_TESTS=ON
make control_plane_rpc_bench

# Run benchmark (simulated mode)
./benchmark/control_plane/control_plane_rpc_bench > baseline_control_plane.txt

# View results
cat baseline_control_plane.txt
```

## Understanding Results

### Key Metrics

1. **RPC Latency**
   - Mean, p50, p99 latencies
   - Throughput (RPC/sec)
   - Breakdown: network vs serialization overhead

2. **sendData/recvData Throughput**
   - Latency by data size
   - Throughput in MB/s
   - Identifies memcpy as bottleneck for large transfers

3. **JSON Overhead**
   - Serialization time per message type
   - Percentage of total RPC time
   - Opportunity for Cap'n Proto

### Expected Baseline Results

```
=== Control Plane RPC Benchmark (Client) ===

--- getSegmentDesc() RPC ---
getSegmentDesc()                   : mean=150.00 μs, p50=145.00 μs, p99=200.00 μs
  Throughput: 6666.67 RPC/sec

--- bootstrap() RPC ---
bootstrap() total                  : mean=180.00 μs, p50=175.00 μs, p99=250.00 μs
  - JSON serialization             : mean=12.00 μs, p50=11.00 μs, p99=18.00 μs
  RPC overhead (network): 168.00 μs
  JSON overhead: 6.67%

--- sendData() / recvData() RPC (HOT PATH) ---
Note: These operations copy data through RPC!

           Size     Mean Latency (μs)     p99 Latency (μs)   Throughput (MB/s)
---------------------------------------------------------------------------
          4 KB               300.00               450.00               13.33
         16 KB               450.00               650.00               35.56
         64 KB               800.00              1100.00               80.00
        256 KB              2500.00              3200.00              102.40
          1 MB              9000.00             11000.00              111.11
          4 MB             35000.00             42000.00              114.29

*** Flow-IPC Opportunity: Zero-copy SHM eliminates memcpy overhead ***

--- JSON Serialization Overhead ---
BootstrapDesc                      : mean=12.00 μs, p50=11.50 μs, p99=18.00 μs
Notification                       : mean=5.00 μs, p50=4.80 μs, p99=8.00 μs
Large JSON (1KB)                   : mean=35.00 μs, p50=33.00 μs, p99=50.00 μs

*** Flow-IPC Opportunity: Cap'n Proto zero-copy replaces JSON ***
```

## Flow-IPC Improvement Targets

### For sendData/recvData (Priority: HIGH)

**Current:**
- 4 MB transfer: ~35 ms
- Dominated by memcpy + network transfer

**With Flow-IPC:**
- 4 MB transfer: ~100-200 μs (just send offset)
- 175x improvement!
- Data stays in SHM, only metadata transmitted

### For JSON Serialization (Priority: MEDIUM)

**Current:**
- BootstrapDesc: ~12 μs
- 6-7% of total RPC time

**With Flow-IPC:**
- Cap'n Proto: <1 μs
- 12x improvement on serialization
- But network RTT (100-200μs) still dominates small messages

### Overall Control Plane (Priority: HIGH for local comms)

**Current (over TCP):**
- RPC latency: 100-500 μs baseline
- sendData copies through RPC

**With Flow-IPC (Unix domain sockets + SHM):**
- RPC latency: ~5-100 μs baseline
- sendData: zero-copy via SHM offsets
- Best for same-machine communication

## Integration with Real RPC Server

To test with actual RPC calls (not simulation):

```bash
# Terminal 1: Start transfer engine as server
./transfer_engine_server --control_port=9000

# Terminal 2: Run benchmark
./control_plane_rpc_bench --server_addr=127.0.0.1:9000
```

## Comparison After Flow-IPC Integration

```bash
# Save baseline
cp baseline_control_plane.txt baseline_control_plane_before.txt

# After Flow-IPC integration
./control_plane_rpc_bench > baseline_control_plane_after.txt

# Compare
diff -u baseline_control_plane_before.txt baseline_control_plane_after.txt
```

## Notes

- This benchmark simulates RPC latency for standalone testing
- For accurate results, integrate with actual RPC server
- sendData/recvData is the biggest optimization opportunity
- Local communication benefits most (same-machine master↔worker)
- Cross-machine RPC still uses TCP (Flow-IPC is local only)
