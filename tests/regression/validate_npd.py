#!/usr/bin/env python3
"""
Validation script for GVFA Null Pointer Dereference detection.
Tests against benchmarks/micro/npd/*.c files.
"""

import os
import subprocess
import sys
import json
from pathlib import Path
from dataclasses import dataclass
from typing import List, Tuple

@dataclass
class TestResult:
    filename: str
    expected_bug: bool
    found_bug: bool
    vuln_count: int
    error: str = ""
    
    @property
    def is_tp(self) -> bool:
        return self.expected_bug and self.found_bug
    
    @property
    def is_tn(self) -> bool:
        return not self.expected_bug and not self.found_bug
    
    @property
    def is_fp(self) -> bool:
        return not self.expected_bug and self.found_bug
    
    @property
    def is_fn(self) -> bool:
        return self.expected_bug and not self.found_bug
    
    @property
    def category(self) -> str:
        if self.error:
            return "ERROR"
        return ("TP" if self.is_tp else "TN" if self.is_tn else 
                "FP" if self.is_fp else "FN" if self.is_fn else "UNKNOWN")


class NPDValidator:
    def __init__(self, 
                 benchmark_dir: str,
                 clang_path: str,
                 lotus_gvfa_path: str,
                 output_dir: str):
        self.benchmark_dir = Path(benchmark_dir)
        self.clang_path = clang_path
        self.lotus_gvfa_path = lotus_gvfa_path
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        
    def check_expected_behavior(self, c_file: Path) -> bool:
        """Check if test file should trigger a null pointer bug."""
        with open(c_file, 'r') as f:
            content = f.read()
        
        filename = c_file.name.lower()
        
        # Files with "safe" in name should not have bugs
        if 'safe_' in filename:
            return 'UNSAFE_LOAD' in content
        
        # Unsafe variants should have bugs
        if 'unsafe' in filename:
            return True
        
        # Check for explicit markers
        if 'UNSAFE_LOAD' in content:
            return True
        if 'SAFE_LOAD' in content and 'UNSAFE_LOAD' not in content:
            return False
        
        # ExtAPI tests with null_ptr should have bugs
        if ('extapi' in filename or 'eexapi' in filename) and 'null_ptr' in filename:
            return True
        
        return True  # Conservative default
    
    def compile_to_bitcode(self, c_file: Path) -> Tuple[Path, str]:
        """Compile C file to LLVM bitcode. Returns (bitcode_path, error_message)."""
        bc_file = self.output_dir / f"{c_file.stem}.bc"
        cmd = [self.clang_path, "-emit-llvm", "-c", "-g", "-O0", 
               str(c_file), "-o", str(bc_file)]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                return bc_file, f"Compilation failed: {result.stderr}"
            return bc_file, ""
        except subprocess.TimeoutExpired:
            return bc_file, "Compilation timeout"
        except Exception as e:
            return bc_file, f"Compilation error: {str(e)}"
    
    def run_gvfa_analysis(self, bc_file: Path) -> Tuple[int, str]:
        """Run GVFA null pointer analysis. Returns (vulnerability_count, error_message)."""
        cmd = [self.lotus_gvfa_path, "--vuln-type=nullpointer", str(bc_file)]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            output = result.stdout + result.stderr
            vuln_count = 0
            
            # Parse output for vulnerability count
            for line in output.split('\n'):
                if 'Found' in line and 'potential vulnerabilities' in line:
                    try:
                        vuln_count = int(line.split()[1])
                    except (ValueError, IndexError):
                        pass
            
            if result.returncode != 0 and vuln_count == 0:
                return 0, f"Analysis failed: {result.stderr[:200]}"
            return vuln_count, ""
            
        except subprocess.TimeoutExpired:
            return 0, "Analysis timeout"
        except Exception as e:
            return 0, f"Analysis error: {str(e)}"
    
    def validate_single_file(self, c_file: Path) -> TestResult:
        """Validate a single test file."""
        print(f"Testing {c_file.name}...", end=" ", flush=True)
        expected_bug = self.check_expected_behavior(c_file)
        
        bc_file, compile_error = self.compile_to_bitcode(c_file)
        if compile_error:
            print("COMPILE ERROR")
            return TestResult(c_file.name, expected_bug, False, 0, compile_error)
        
        vuln_count, analysis_error = self.run_gvfa_analysis(bc_file)
        if analysis_error:
            print("ANALYSIS ERROR")
            return TestResult(c_file.name, expected_bug, False, 0, analysis_error)
        
        found_bug = vuln_count > 0
        result = TestResult(c_file.name, expected_bug, found_bug, vuln_count)
        print(f"{result.category} (expected={expected_bug}, found={found_bug}, count={vuln_count})")
        return result
    
    def validate_all(self) -> List[TestResult]:
        """Validate all test files in the benchmark directory."""
        c_files = sorted(self.benchmark_dir.glob("*.c"))
        if not c_files:
            print(f"Error: No .c files found in {self.benchmark_dir}")
            return []
        
        print(f"Found {len(c_files)} test files\n{'='*80}")
        return [self.validate_single_file(c_file) for c_file in c_files]
    
    def print_summary(self, results: List[TestResult]):
        """Print validation summary with statistics."""
        tp = [r for r in results if r.is_tp]
        tn = [r for r in results if r.is_tn]
        fp = [r for r in results if r.is_fp]
        fn = [r for r in results if r.is_fn]
        errors = [r for r in results if r.error]
        
        total = len(results)
        correct = len(tp) + len(tn)
        
        print(f"\n{'='*80}\nVALIDATION SUMMARY\n{'='*80}")
        print(f"\nTotal: {total} | Correct: {correct} ({100*correct/total:.1f}%) | Errors: {len(errors)}")
        print(f"\nBreakdown:")
        print(f"  TP: {len(tp):3d}  TN: {len(tn):3d}  FP: {len(fp):3d}  FN: {len(fn):3d}")
        
        # Metrics
        if len(tp) + len(fp) > 0 and len(tp) + len(fn) > 0:
            precision = len(tp) / (len(tp) + len(fp))
            recall = len(tp) / (len(tp) + len(fn))
            f1 = 2 * (precision * recall) / (precision + recall) if precision + recall > 0 else 0
            print(f"\nPrecision: {precision:.2%}  Recall: {recall:.2%}  F1: {f1:.2%}")
        
        # List problematic cases
        for category, items in [("FALSE POSITIVES", fp), ("FALSE NEGATIVES", fn), ("ERRORS", errors)]:
            if items:
                print(f"\n{'='*80}\n{category} ({len(items)}):\n{'='*80}")
                for r in items:
                    extra = f" (count={r.vuln_count})" if category == "FALSE POSITIVES" else f" - {r.error[:60]}" if category == "ERRORS" else ""
                    print(f"  {r.filename:50s}{extra}")
        
        print(f"\n{'='*80}")
    
    def save_results_json(self, results: List[TestResult], output_file: str):
        """Save results to JSON file."""
        data = {
            'total': len(results),
            'true_positives': sum(1 for r in results if r.is_tp),
            'true_negatives': sum(1 for r in results if r.is_tn),
            'false_positives': sum(1 for r in results if r.is_fp),
            'false_negatives': sum(1 for r in results if r.is_fn),
            'errors': sum(1 for r in results if r.error),
            'results': [{
                'filename': r.filename,
                'expected_bug': r.expected_bug,
                'found_bug': r.found_bug,
                'vuln_count': r.vuln_count,
                'category': r.category,
                'error': r.error
            } for r in results]
        }
        
        with open(output_file, 'w') as f:
            json.dump(data, f, indent=2)
        print(f"Results saved to: {output_file}")


def main():
    # Configuration - use relative paths and environment variables
    script_dir = Path(__file__).parent
    benchmark_dir = script_dir / "benchmarks/micro/npd"
    clang_path = os.getenv("CLANG", "clang")  # Use system clang by default
    lotus_gvfa_path = script_dir / "build/bin/lotus-gvfa"
    output_dir = script_dir / "npd_validation_results"
    
    # Verify required paths exist
    if not benchmark_dir.exists():
        print(f"Error: Benchmark directory not found: {benchmark_dir}")
        return 1
    
    if not lotus_gvfa_path.exists():
        print(f"Error: lotus-gvfa not found: {lotus_gvfa_path}")
        print("Please build the project first.")
        return 1
    
    print(f"NPD Benchmark Validation\n{'='*80}")
    print(f"Benchmark dir: {benchmark_dir}")
    print(f"Clang:         {clang_path}")
    print(f"GVFA tool:     {lotus_gvfa_path}")
    print(f"Output dir:    {output_dir}\n{'='*80}\n")
    
    # Run validation
    validator = NPDValidator(
        benchmark_dir=str(benchmark_dir),
        clang_path=str(clang_path),
        lotus_gvfa_path=str(lotus_gvfa_path),
        output_dir=str(output_dir)
    )
    
    results = validator.validate_all()
    if not results:
        print("No results to report.")
        return 1
    
    validator.print_summary(results)
    validator.save_results_json(results, str(output_dir / "validation_results.json"))
    
    return 1 if any(r.is_fp or r.is_fn or r.error for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())

