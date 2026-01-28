# Performance Testing & Comparison Framework

Automated tools for benchmarking, regression detection, and performance validation.

## Overview

This framework provides:
- **Automated benchmarking**: Run all benchmarks with one command
- **Regression detection**: Automatically detect performance degradations
- **Visual reports**: Generate HTML reports with charts
- **CI/CD integration**: GitHub Actions workflow for continuous testing

## Quick Start

### Run All Benchmarks

```bash
# Build and run all benchmarks
./scripts/build_benchmarks.sh
./scripts/run_all_benchmarks.sh

# Results saved in: benchmark_results/baseline/
```

### Compare Performance

```bash
# Generate comparison report
./scripts/benchmark_comparison.sh

# View report
cat benchmark_results/comparison/comparison_*.txt
```

### Run Regression Tests

```bash
# Run benchmarks and save baseline
python3 scripts/regression_test.py \
    --output baseline.json

# Run again after changes
python3 scripts/regression_test.py \
    --output current.json \
    --baseline baseline.json

# Compare two runs
python3 scripts/regression_test.py \
    --compare baseline.json current.json
```

### Generate Visual Report

```bash
python3 scripts/generate_performance_report.py \
    --output performance_report.html

# Open in browser
firefox performance_report.html
```

## Tools

### 1. benchmark_comparison.sh

Automated comparison of original vs arena implementation.

**Usage**:
```bash
./scripts/benchmark_comparison.sh
```

**Output**:
- Comparison report in `benchmark_results/comparison/`
- Side-by-side performance metrics
- Speedup calculations
- Bottleneck analysis

**Example Output**:
```
================================================
ALLOCATION PERFORMANCE
================================================

Original: 10,220 ns
Arena:       142 ns
Improvement: 72x faster ✅

================================================
ADDRESS TRANSLATION PERFORMANCE
================================================

Original: 487 ns (O(n))
Arena:      5 ns (O(1))
Improvement: 97x faster ✅
```

### 2. regression_test.py

Python-based regression testing framework.

**Features**:
- Runs all benchmarks automatically
- Parses results and extracts metrics
- Compares against baseline
- Detects regressions (configurable thresholds)
- JSON output for CI/CD integration

**Usage**:

```bash
# Basic run
python3 scripts/regression_test.py

# Save results
python3 scripts/regression_test.py --output results.json

# Compare with baseline
python3 scripts/regression_test.py \
    --output current.json \
    --baseline baseline.json

# CI mode (fails on regression)
python3 scripts/regression_test.py \
    --baseline baseline.json \
    --ci
```

**Configuration**:

Edit thresholds in `regression_test.py`:
```python
THRESHOLDS = {
    'allocation_total_time': 10.0,  # 10% slowdown = regression
    'allocation_throughput': -10.0,  # 10% drop = regression
    'lookup_linear_scan_time': 10.0,
    'lookup_arithmetic_time': 10.0,
    'transfer_1mb_bandwidth': -10.0,
}
```

**Output Format**:

JSON results file:
```json
{
  "timestamp": "2026-01-28T12:34:56",
  "metrics": {
    "allocation_total_time": {
      "name": "allocation_total_time",
      "value": 10220.0,
      "unit": "nanoseconds"
    },
    ...
  },
  "metadata": {
    "build_dir": "/path/to/build",
    "git_commit": "abc123..."
  }
}
```

### 3. generate_performance_report.py

Creates visual HTML reports with interactive charts.

**Usage**:
```bash
python3 scripts/generate_performance_report.py \
    --output report.html
```

**Features**:
- Interactive bar charts (Chart.js)
- Allocation comparison
- Lookup comparison
- Complexity scaling visualization (O(n) vs O(1))
- Summary cards with key metrics
- Professional styling

**Report Includes**:
- Summary metrics (100x, 392x improvements)
- Visual comparisons
- Complexity analysis
- Key improvements list

### 4. GitHub Actions Workflow

Automated CI/CD integration for performance testing.

**Location**: `.github/workflows/performance_regression.yml`

**Triggers**:
- Pull requests to main/develop
- Manual dispatch
- Push to main (updates baseline)

**Steps**:
1. Build benchmarks
2. Download baseline (if exists)
3. Run current benchmarks
4. Compare with baseline
5. Post results as PR comment
6. Fail build if regression detected
7. Upload new baseline (on main branch)

**PR Comment Example**:
```
## Performance Regression Test Results

================================================
Performance Regression Report
================================================
Baseline: 2026-01-28T10:00:00
Current:  2026-01-28T12:00:00

✅ No regressions detected!

All metrics within acceptable thresholds.
================================================
```

## Benchmark Metrics

### Tracked Metrics

| Metric | Description | Unit | Threshold |
|--------|-------------|------|-----------|
| `allocation_total_time` | Time for shm_open+ftruncate+mmap | ns | ±10% |
| `allocation_throughput` | Allocations per second | alloc/s | ±10% |
| `lookup_linear_scan_time` | O(n) address lookup | ns | ±10% |
| `lookup_arithmetic_time` | O(1) address lookup | ns | ±10% |
| `transfer_1mb_bandwidth` | Memory transfer speed | GB/s | ±10% |

### Expected Values

**Original Implementation**:
- Allocation: ~10,000 - 50,000 ns
- Lookup (100 segments): ~500 ns
- Transfer (1MB): ~30-60 GB/s

**Arena Implementation**:
- Allocation: ~100-500 ns (100x faster)
- Lookup: ~5 ns (100x faster)
- Transfer: ~30-60 GB/s (same)

## Integration with CI/CD

### GitHub Actions

The workflow automatically:
1. Runs on every PR
2. Compares performance against baseline
3. Posts results as PR comment
4. Fails build if >10% regression
5. Updates baseline on merge to main

**Enable in your repo**:
```bash
# Workflow already created at:
.github/workflows/performance_regression.yml

# GitHub Actions will run automatically
```

### Jenkins Integration

```groovy
pipeline {
    stages {
        stage('Performance Test') {
            steps {
                sh './scripts/build_benchmarks.sh'
                sh 'python3 scripts/regression_test.py --ci --baseline baseline.json'
            }
        }
    }
    post {
        always {
            archiveArtifacts 'benchmark_results/**'
        }
    }
}
```

### GitLab CI

```yaml
performance_test:
  script:
    - ./scripts/build_benchmarks.sh
    - python3 scripts/regression_test.py --ci --baseline baseline.json
  artifacts:
    paths:
      - benchmark_results/
    reports:
      metrics: results.json
```

## Troubleshooting

### Benchmarks Fail to Build

```bash
# Check dependencies
sudo apt-get install -y libgtest-dev libgflags-dev libgoogle-glog-dev

# Clean build
rm -rf build
./scripts/build_benchmarks.sh
```

### Permission Denied on /dev/shm

```bash
# Fix SHM permissions
sudo chmod 777 /dev/shm

# Or run with sudo (not recommended for production)
sudo ./scripts/run_all_benchmarks.sh
```

### Inconsistent Results

Benchmarks can be affected by:
- CPU frequency scaling
- Background processes
- Thermal throttling

**Solutions**:
```bash
# Disable CPU frequency scaling
sudo cpupower frequency-set --governor performance

# Isolate CPU cores
taskset -c 0-3 ./scripts/run_all_benchmarks.sh

# Run multiple times and average
for i in {1..5}; do
    python3 scripts/regression_test.py --output run_$i.json
done
```

### Clean Up SHM Segments

```bash
# List all Mooncake SHM segments
ls -lh /dev/shm/mooncake_*

# Remove all
rm /dev/shm/mooncake_*
```

## Best Practices

### Before/After Comparison

```bash
# 1. Establish baseline (before changes)
git checkout main
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py --output baseline.json

# 2. Make changes
git checkout my-feature-branch
# ... make changes ...

# 3. Test changes
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py \
    --output current.json \
    --baseline baseline.json

# 4. Generate report
./scripts/benchmark_comparison.sh
python3 scripts/generate_performance_report.py
```

### Continuous Monitoring

```bash
# Daily cron job
0 2 * * * cd /path/to/mooncake && \
    ./scripts/build_benchmarks.sh && \
    python3 scripts/regression_test.py \
        --output /var/log/mooncake/daily_$(date +\%Y\%m\%d).json
```

### A/B Testing

```bash
# Test two implementations
# Implementation A
git checkout feature-A
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py --output impl_A.json

# Implementation B
git checkout feature-B
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py --output impl_B.json

# Compare
python3 scripts/regression_test.py --compare impl_A.json impl_B.json
```

## Example Workflow

### Developer Workflow

```bash
# 1. Start work on new feature
git checkout -b optimize-allocation

# 2. Run baseline benchmarks
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py --output before.json

# 3. Make changes
vim mooncake-transfer-engine/tent/src/transport/shm/shm_transport.cpp

# 4. Test changes
./scripts/build_benchmarks.sh
python3 scripts/regression_test.py \
    --output after.json \
    --baseline before.json

# 5. Generate report for review
python3 scripts/generate_performance_report.py \
    --output pr_report.html

# 6. Commit with report
git add .
git commit -m "Optimize allocation - 100x improvement (see pr_report.html)"
git push origin optimize-allocation
```

### Code Review Process

1. **PR Created**: GitHub Actions automatically runs benchmarks
2. **Results Posted**: Bot comments on PR with comparison
3. **Review**: Reviewer checks performance metrics
4. **Approve/Request Changes**: Based on regression status
5. **Merge**: Baseline updated automatically

## File Locations

```
mooncake/
├── .github/workflows/
│   └── performance_regression.yml  # CI/CD workflow
├── scripts/
│   ├── benchmark_comparison.sh      # Comparison script
│   ├── regression_test.py           # Regression testing
│   ├── generate_performance_report.py  # Visual reports
│   ├── build_benchmarks.sh          # Build script
│   ├── run_all_benchmarks.sh        # Run script
│   └── README_TESTING.md            # This file
└── benchmark_results/
    ├── baseline/                    # Baseline results
    ├── arena/                       # Arena results
    └── comparison/                  # Comparison reports
```

## FAQ

**Q: How often should I run benchmarks?**
A: Run before/after changes, and automatically on every PR.

**Q: What threshold should I use for regressions?**
A: Default is 10%. Adjust based on your requirements.

**Q: Can I benchmark on Mac/Windows?**
A: SHM benchmarks are Linux-specific. Use Linux or WSL2.

**Q: How to compare against different baselines?**
A: Save multiple baselines and specify which to compare against:
```bash
python3 scripts/regression_test.py \
    --baseline old_baseline.json \
    --output current.json
```

**Q: How to disable specific benchmarks?**
A: Edit the benchmark runner in `regression_test.py` and comment out unwanted benchmarks.

---

**Last Updated**: 2026-01-28
**Status**: Production Ready
