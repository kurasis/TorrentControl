"""Require real ASan diagnoses from two isolated, deliberately faulty probes."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--probe", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
args = parser.parse_args()
args.report.parent.mkdir(parents=True, exist_ok=True)
report = {"scope": "ASan runtime negative controls, not application failures", "passed": False, "controls": []}
environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:exitcode=86")
try:
    for mode, expected in [("heap-overflow", "heap-buffer-overflow"), ("use-after-free", "heap-use-after-free")]:
        result = subprocess.run([str(args.probe.resolve()), mode], env=environment, capture_output=True,
                                text=True, errors="replace", timeout=30)
        output = result.stdout + result.stderr
        (args.report.parent / (mode + ".log")).write_text(output, encoding="utf-8")
        passed = result.returncode != 0 and "AddressSanitizer" in output and expected in output
        report["controls"].append({"mode": mode, "exitCode": result.returncode,
                                    "requiredDiagnostic": expected, "passed": passed})
        if not passed:
            raise RuntimeError(f"ASan failed to diagnose {mode}: exit={result.returncode}; see retained log")
    report["passed"] = True
finally:
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report))
