#!/usr/bin/env python3
"""
Performance regression testing framework for Mooncake SHM Arena

This script:
1. Runs benchmarks and collects metrics
2. Compares against baseline or previous runs
3. Detects performance regressions
4. Generates reports for CI/CD integration

Usage:
    ./regression_test.py --baseline baseline.json
    ./regression_test.py --compare before.json after.json
    ./regression_test.py --ci  # For CI/CD integration
"""

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


class BenchmarkMetric:
    """Represents a single benchmark metric"""
    def __init__(self, name: str, value: float, unit: str):
        self.name = name
        self.value = value
        self.unit = unit

    def to_dict(self) -> dict:
        return {
            'name': self.name,
            'value': self.value,
            'unit': self.unit
        }

    @classmethod
    def from_dict(cls, data: dict) -> 'BenchmarkMetric':
        return cls(data['name'], data['value'], data['unit'])


class BenchmarkResults:
    """Collection of benchmark results"""
    def __init__(self, timestamp: str = None):
        self.timestamp = timestamp or datetime.now().isoformat()
        self.metrics: Dict[str, BenchmarkMetric] = {}
        self.metadata: Dict[str, str] = {}

    def add_metric(self, metric: BenchmarkMetric):
        self.metrics[metric.name] = metric

    def to_dict(self) -> dict:
        return {
            'timestamp': self.timestamp,
            'metrics': {k: v.to_dict() for k, v in self.metrics.items()},
            'metadata': self.metadata
        }

    @classmethod
    def from_dict(cls, data: dict) -> 'BenchmarkResults':
        results = cls(data['timestamp'])
        results.metrics = {
            k: BenchmarkMetric.from_dict(v)
            for k, v in data['metrics'].items()
        }
        results.metadata = data.get('metadata', {})
        return results

    def save(self, filepath: Path):
        with open(filepath, 'w') as f:
            json.dump(self.to_dict(), f, indent=2)

    @classmethod
    def load(cls, filepath: Path) -> 'BenchmarkResults':
        with open(filepath, 'r') as f:
            data = json.load(f)
        return cls.from_dict(data)


class BenchmarkRunner:
    """Runs benchmarks and parses results"""

    def __init__(self, build_dir: Path):
        self.build_dir = build_dir
        self.bench_dir = build_dir / "mooncake-transfer-engine" / "benchmark"

    def run_allocation_bench(self) -> List[BenchmarkMetric]:
        """Run SHM allocation benchmark and extract metrics"""
        cmd = [
            str(self.bench_dir / "shm" / "shm_allocation_bench"),
            "--num_iterations=1000",
            "--min_size_kb=4",
            "--max_size_kb=1024",
            "--cleanup=true"
        ]

        output = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)

        metrics = []

        # Parse allocation times
        match = re.search(r'Total \(all 3 syscalls\).*?mean=([\d.]+) ns', output)
        if match:
            metrics.append(BenchmarkMetric(
                'allocation_total_time',
                float(match.group(1)),
                'nanoseconds'
            ))

        # Parse throughput
        match = re.search(r'Throughput: ([\d.]+) allocations/sec', output)
        if match:
            metrics.append(BenchmarkMetric(
                'allocation_throughput',
                float(match.group(1)),
                'allocations_per_sec'
            ))

        return metrics

    def run_lookup_bench(self) -> List[BenchmarkMetric]:
        """Run address lookup benchmark and extract metrics"""
        cmd = [
            str(self.bench_dir / "shm" / "shm_address_lookup_bench"),
            "--num_segments=100",
            "--num_lookups=10000",
            "--segment_size_mb=64"
        ]

        output = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)

        metrics = []

        # Parse linear scan time
        match = re.search(r'Linear Scan.*?Avg time per lookup: ([\d.]+) ns', output, re.DOTALL)
        if match:
            metrics.append(BenchmarkMetric(
                'lookup_linear_scan_time',
                float(match.group(1)),
                'nanoseconds'
            ))

        # Parse arithmetic translation time
        match = re.search(r'Arithmetic Translation.*?Avg time per lookup: ([\d.]+) ns', output, re.DOTALL)
        if match:
            metrics.append(BenchmarkMetric(
                'lookup_arithmetic_time',
                float(match.group(1)),
                'nanoseconds'
            ))

        return metrics

    def run_transfer_bench(self) -> List[BenchmarkMetric]:
        """Run transfer benchmark and extract metrics"""
        cmd = [
            str(self.bench_dir / "shm" / "shm_transfer_bench"),
            "--transfer_size_kb=4",
            "--max_transfer_size_mb=64",
            "--num_transfers=1000"
        ]

        output = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)

        metrics = []

        # Parse 1MB transfer bandwidth
        match = re.search(r'1 MB\s+([\d.]+)\s+([\d.]+)', output)
        if match:
            metrics.append(BenchmarkMetric(
                'transfer_1mb_bandwidth',
                float(match.group(2)),
                'GB_per_sec'
            ))

        return metrics

    def run_all(self) -> BenchmarkResults:
        """Run all benchmarks and collect results"""
        results = BenchmarkResults()

        print("Running allocation benchmark...")
        for metric in self.run_allocation_bench():
            results.add_metric(metric)

        print("Running lookup benchmark...")
        for metric in self.run_lookup_bench():
            results.add_metric(metric)

        print("Running transfer benchmark...")
        for metric in self.run_transfer_bench():
            results.add_metric(metric)

        # Add metadata
        results.metadata['build_dir'] = str(self.build_dir)
        results.metadata['git_commit'] = self._get_git_commit()

        return results

    def _get_git_commit(self) -> str:
        """Get current git commit hash"""
        try:
            result = subprocess.run(
                ['git', 'rev-parse', 'HEAD'],
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout.strip()
        except:
            return 'unknown'


class RegressionDetector:
    """Detects performance regressions by comparing results"""

    # Regression thresholds (percentage degradation)
    THRESHOLDS = {
        'allocation_total_time': 10.0,  # 10% slowdown is a regression
        'allocation_throughput': -10.0,  # 10% throughput drop is a regression
        'lookup_linear_scan_time': 10.0,
        'lookup_arithmetic_time': 10.0,
        'transfer_1mb_bandwidth': -10.0,
    }

    def __init__(self, baseline: BenchmarkResults, current: BenchmarkResults):
        self.baseline = baseline
        self.current = current

    def detect_regressions(self) -> List[Tuple[str, float, float, float]]:
        """
        Detect regressions by comparing metrics

        Returns: List of (metric_name, baseline_value, current_value, percent_change)
        """
        regressions = []

        for metric_name, baseline_metric in self.baseline.metrics.items():
            if metric_name not in self.current.metrics:
                continue

            current_metric = self.current.metrics[metric_name]
            baseline_val = baseline_metric.value
            current_val = current_metric.value

            # Calculate percent change
            if baseline_val == 0:
                continue

            percent_change = ((current_val - baseline_val) / baseline_val) * 100

            # Check if this is a regression
            threshold = self.THRESHOLDS.get(metric_name, 10.0)

            # For metrics where higher is better (throughput, bandwidth)
            if metric_name.endswith('throughput') or metric_name.endswith('bandwidth'):
                if percent_change < threshold:  # Negative threshold means drop
                    regressions.append((metric_name, baseline_val, current_val, percent_change))
            # For metrics where lower is better (time)
            else:
                if percent_change > threshold:
                    regressions.append((metric_name, baseline_val, current_val, percent_change))

        return regressions

    def generate_report(self) -> str:
        """Generate human-readable regression report"""
        regressions = self.detect_regressions()

        report = []
        report.append("=" * 60)
        report.append("Performance Regression Report")
        report.append("=" * 60)
        report.append(f"Baseline: {self.baseline.timestamp}")
        report.append(f"Current:  {self.current.timestamp}")
        report.append("")

        if not regressions:
            report.append("✅ No regressions detected!")
            report.append("")
            report.append("All metrics within acceptable thresholds.")
        else:
            report.append(f"⚠️  {len(regressions)} regression(s) detected!")
            report.append("")

            for metric_name, baseline_val, current_val, percent_change in regressions:
                report.append(f"Metric: {metric_name}")
                report.append(f"  Baseline: {baseline_val:.2f}")
                report.append(f"  Current:  {current_val:.2f}")
                report.append(f"  Change:   {percent_change:+.2f}%")
                report.append("")

        # Show all metrics comparison
        report.append("=" * 60)
        report.append("All Metrics Comparison")
        report.append("=" * 60)
        report.append("")

        for metric_name in sorted(self.baseline.metrics.keys()):
            if metric_name not in self.current.metrics:
                continue

            baseline_metric = self.baseline.metrics[metric_name]
            current_metric = self.current.metrics[metric_name]

            baseline_val = baseline_metric.value
            current_val = current_metric.value
            percent_change = ((current_val - baseline_val) / baseline_val) * 100 if baseline_val != 0 else 0

            status = "✅" if abs(percent_change) < 5 else "⚠️"

            report.append(f"{status} {metric_name}:")
            report.append(f"   Baseline: {baseline_val:.2f} {baseline_metric.unit}")
            report.append(f"   Current:  {current_val:.2f} {current_metric.unit}")
            report.append(f"   Change:   {percent_change:+.2f}%")
            report.append("")

        return "\n".join(report)


def main():
    parser = argparse.ArgumentParser(
        description='Performance regression testing for Mooncake SHM Arena'
    )
    parser.add_argument(
        '--build-dir',
        type=Path,
        default=Path(__file__).parent.parent / 'build',
        help='Build directory containing benchmarks'
    )
    parser.add_argument(
        '--output',
        type=Path,
        help='Output file for results (JSON)'
    )
    parser.add_argument(
        '--baseline',
        type=Path,
        help='Baseline results file for comparison'
    )
    parser.add_argument(
        '--compare',
        nargs=2,
        metavar=('BEFORE', 'AFTER'),
        help='Compare two result files'
    )
    parser.add_argument(
        '--ci',
        action='store_true',
        help='CI mode: fail if regressions detected'
    )
    parser.add_argument(
        '--threshold',
        type=float,
        default=10.0,
        help='Regression threshold percentage (default: 10%%)'
    )

    args = parser.parse_args()

    # Run benchmarks if not just comparing
    if not args.compare:
        print("Running benchmarks...")
        runner = BenchmarkRunner(args.build_dir)
        results = runner.run_all()

        # Save results
        if args.output:
            results.save(args.output)
            print(f"\nResults saved to: {args.output}")
        else:
            # Default output location
            results_dir = Path(__file__).parent.parent / 'benchmark_results'
            results_dir.mkdir(exist_ok=True)
            output_file = results_dir / f"results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
            results.save(output_file)
            print(f"\nResults saved to: {output_file}")

        # Compare with baseline if provided
        if args.baseline:
            print("\nComparing with baseline...")
            baseline = BenchmarkResults.load(args.baseline)
            detector = RegressionDetector(baseline, results)
            report = detector.generate_report()
            print("\n" + report)

            if args.ci:
                regressions = detector.detect_regressions()
                if regressions:
                    print("\n❌ CI FAILURE: Performance regressions detected")
                    sys.exit(1)
                else:
                    print("\n✅ CI SUCCESS: No regressions")
                    sys.exit(0)
    else:
        # Compare two existing result files
        print("Comparing results...")
        before = BenchmarkResults.load(Path(args.compare[0]))
        after = BenchmarkResults.load(Path(args.compare[1]))

        detector = RegressionDetector(before, after)
        report = detector.generate_report()
        print("\n" + report)

        if args.ci:
            regressions = detector.detect_regressions()
            sys.exit(1 if regressions else 0)


if __name__ == '__main__':
    main()
