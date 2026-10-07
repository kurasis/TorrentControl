"""Run bounded, reproducible libFuzzer campaigns and retain logs/crash inputs."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True, type=Path)
parser.add_argument("--corpus", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--runs", type=int, default=1)
parser.add_argument("--seeds", type=int, nargs="+", default=[1, 7, 42])
args = parser.parse_args()
if args.runs < 1 or not args.seeds or any(seed < 1 for seed in args.seeds):
    parser.error("runs and seeds must be positive")
args.output.mkdir(parents=True, exist_ok=True)
report = {"platform": platform.platform(), "scope": "project-code-only; parser/edit/JSON/bridge/URL harnesses; no network or payload I/O",
          "maxInputBytes": 65536, "metainfoDepth": 64, "metainfoNodes": 4096,
          "jsonDepth": 64, "productionJsonDepth": 512, "perInputTimeoutSeconds": 5, "rssLimitMiB": 1024,
          "originalRunsPerCampaign": 3,
          "runsPerCampaign": args.runs, "seeds": args.seeds,
          "mode": "smoke" if args.runs <= 3 else "bounded-fuzz",
          "initialCorpusReplayMayExceedRuns": True, "campaigns": [], "campaignPassed": False}
cache = args.build / "CMakeCache.txt"
if cache.exists():
    report["cmake"] = {line.split(":", 1)[0]: line.split("=", 1)[1]
                       for line in cache.read_text().splitlines()
                       if re.match(r"^(CMAKE_CXX_COMPILER|TC_ENABLE_SANITIZERS|TC_BUILD_FUZZERS):", line)}
    compiler = report["cmake"].get("CMAKE_CXX_COMPILER")
    if compiler:
        report["compilerVersion"] = subprocess.run([compiler, "--version"], check=True,
                                                   capture_output=True, text=True, timeout=10).stdout.strip()
environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")


def save():
    (args.output / "campaign.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


save()
for target in ["metainfo", "tagged_json", "bridge_input", "network_url"]:
    binary = (args.build / "tests/fuzz" / f"tc-fuzz-{target}").resolve()
    for seed in args.seeds:
        name = f"{target}-{seed}"
        work = args.output / name
        if work.exists():
            raise SystemExit(f"Campaign output already exists: {work}; choose a fresh output directory")
        corpus = work / "corpus"
        shutil.copytree(args.corpus / target, corpus)
        artifacts = work / "artifacts"
        artifacts.mkdir()
        command = [str(binary), str(corpus.resolve()), f"-runs={args.runs}", f"-seed={seed}",
                   "-max_len=65536", "-timeout=5", "-rss_limit_mb=1024", "-print_final_stats=1",
                   f"-artifact_prefix={artifacts.resolve()}/"]
        started = time.monotonic()
        entry = {"target": target, "seed": seed, "command": command, "initialCorpusFiles": len(list(corpus.iterdir()))}
        try:
            with (args.output / f"{name}.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment, timeout=600)
            entry["exitCode"] = result.returncode
            text = (args.output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
            stats = dict(re.findall(r"stat::\s*(\w+):\s+(\d+)", text))
            entry["stats"] = {key: int(value) for key, value in stats.items()}
            entry["passed"] = result.returncode == 0 and entry["stats"].get("number_of_executed_units", 0) >= args.runs
        except subprocess.TimeoutExpired:
            entry.update({"passed": False, "failure": "campaign exceeded 600 seconds"})
        entry["elapsedSeconds"] = time.monotonic() - started
        entry["artifacts"] = [path.name for path in artifacts.iterdir()]
        report["campaigns"].append(entry)
        save()
        print(json.dumps(entry), flush=True)
        if not entry["passed"]:
            raise SystemExit(f"Fuzz campaign failed: {name}; inspect retained log and artifacts")
report["campaignPassed"] = True
save()
