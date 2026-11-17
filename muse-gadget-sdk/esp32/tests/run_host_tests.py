#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The fast standard flow for the esp32 host tests.

    python3 tests/run_host_tests.py                 # full run, diff vs baseline
    python3 tests/run_host_tests.py --jobs 1        # serial (same machinery)
    python3 tests/run_host_tests.py --only battery  # substring filter on test ids
    python3 tests/run_host_tests.py --update-baseline

Why this exists (measured 2026-11-17 on the dev VM): a serial
`python3 -m unittest discover` run takes ~30 min, and the old verification
flow ran it twice (pristine baseline worktree + modified tree) — about an
hour per firmware change. This runner instead:

* runs the suite ONCE, split across worker processes. Units are test
  classes that define setUpClass (their harness compiles once, exactly as
  serial) and single tests everywhere else, longest-expected units first.
  test_avatar_install alone is ~26 min serial (tools/muse/avatar.py's
  host_check renders 2100 animation frames plus GIF previews per install);
  per-test units let it spread across workers.
* diffs the FAIL/ERROR set against the recorded baseline in
  tests/host_test_baseline.txt. New failures fail the run (exit 1);
  baseline entries that now pass are reported as FIXED (exit stays 0).
  Re-derive the baseline with --update-baseline ONLY when test code or
  harnesses change, or the environment changes (see the baseline header).
* points TMPDIR at a disk-backed directory (default
  ~/workspace/.tmp-tests/host-tests-tmp, override with --tmpdir or
  $HOST_TEST_TMPDIR), with a private subdirectory per worker. The suite's
  temp usage peaks over 600 MB for one worker; the VM's /tmp is a 512 MB
  tmpfs, and overflowing it produced phantom compile errors and forced
  re-runs in dev.22 and dev.23.
* survives this VM's storage write limiter. The sandbox mediating the
  workspace denies file creates with EPERM ("Operation not permitted")
  once writes run too hot — measured 2026-11-17: ~318k rapid
  create/delete cycles trip it, writes then fail workspace-wide, and it
  recovers only after a quiet period (writes have also been seen to
  land as 0-byte files under load). A serial run stays under the limit;
  parallel workers burst past it (anim frame dumps, tree copies), and a
  tripped run corrupts hundreds of results (failures=236 where the
  serial truth is 13). So --jobs defaults to 1 (the reliable pace on
  this VM); higher values are there for subset runs and quieter days.
  Either way, any unit whose results carry an EPERM trace is re-run
  serially once writes work again (up to three rounds, probe-gated),
  and its fresh results replace the tainted ones; the runner warns
  when results still carry EPERM traces after that, and refuses to
  derive a baseline from a tainted run.

Exit codes: 0 = no new failures, 1 = new failures vs baseline,
2 = the run itself broke (a worker died, discovery failed, ...).
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import shutil
import sys
import tempfile
import time
import traceback
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
DEFAULT_BASELINE = TESTS_DIR / "host_test_baseline.txt"


# --------------------------------------------------------------------------
# Result collection (shared by workers)
# --------------------------------------------------------------------------

class Collector(unittest.TestResult):
    """Records one row per test outcome, keyed the way the baseline is:
    str(test) — "test_x (module.Class.test_x)", "setUpClass (module.Class)",
    and for subtests "test_x (module.Class.test_x) (param=value, ...)".
    Subtest failures arrive via addSubTest, not addFailure (Python 3.12),
    so both are overridden."""

    def __init__(self):
        super().__init__()
        self.rows = []
        self._t0 = None

    def startTest(self, test):
        super().startTest(test)
        self._t0 = time.perf_counter()

    def _row(self, test, status, err=None):
        dur = time.perf_counter() - self._t0 if self._t0 else 0.0
        row = {"key": str(test), "id": test.id(), "status": status,
               "dur": round(dur, 3)}
        if err is not None:
            row["trace"] = self._exc_info_to_string(err, test)
        self.rows.append(row)

    def addSuccess(self, test):
        super().addSuccess(test)
        self._row(test, "ok")

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._row(test, "FAIL", err)

    def addError(self, test, err):
        super().addError(test, err)
        self._row(test, "ERROR", err)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._row(test, "skipped")

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            self._row(subtest, "FAIL", err)


# --------------------------------------------------------------------------
# Work units
# --------------------------------------------------------------------------

def _defines_setup_class(cls) -> bool:
    return any("setUpClass" in vars(base) for base in cls.__mro__
               if base is not unittest.TestCase)


def enumerate_units(filters):
    """Discover the suite and group test ids into units: a whole class when
    the class defines setUpClass (compile-once fixtures stay compile-once),
    otherwise one unit per test (setUp is per-test by definition there)."""
    loader = unittest.TestLoader()
    suite = loader.discover(str(TESTS_DIR), pattern="test_*.py",
                            top_level_dir=str(TESTS_DIR))
    by_class = {}
    singles = []

    def walk(node):
        if isinstance(node, unittest.TestSuite):
            for child in node:
                walk(child)
        else:
            by_class.setdefault(type(node), []).append(node.id())

    walk(suite)
    units = []
    for cls, ids in by_class.items():
        if _defines_setup_class(cls) and len(ids) > 1:
            units.append(sorted(ids))
        else:
            units.extend([i] for i in sorted(ids))
    if filters:
        units = [u for u in units
                 if any(any(f in i for f in filters) for i in u)]
        units = [[i for i in u if any(f in i for f in filters)]
                 for u in units]
    # Longest-expected first (LPT): test_avatar_install's per-test units are
    # the long pole (~48 s per successful install), then bigger class units.
    units.sort(key=lambda u: (0 if u[0].startswith("test_avatar_install.")
                              else 1, -len(u)))
    return units


# --------------------------------------------------------------------------
# Worker
# --------------------------------------------------------------------------

def _worker_init(tmpdir):
    os.chdir(TESTS_DIR)
    if str(TESTS_DIR) not in sys.path:
        sys.path.insert(0, str(TESTS_DIR))
    # A private temp dir per worker, so parallel workers never share a
    # temp tree (mkdtemp collisions and footprint stay per-worker).
    mine = os.path.join(tmpdir, f"worker-{os.getpid()}")
    os.makedirs(mine, exist_ok=True)
    os.environ["TMPDIR"] = mine
    tempfile.tempdir = mine


def _run_unit(job):
    idx, ids = job
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for tid in ids:
        suite.addTests(loader.loadTestsFromName(tid))
    result = Collector()
    t0 = time.time()
    try:
        suite.run(result)
    except Exception:
        return {"idx": idx, "ids": ids, "rows": [], "testsRun": 0,
                "fatal": traceback.format_exc()}
    return {"idx": idx, "ids": ids, "rows": result.rows,
            "testsRun": result.testsRun, "wall": round(time.time() - t0, 1)}


def _eperm_tainted(outcome):
    return any("Operation not permitted" in r.get("trace", "")
               for r in outcome["rows"])


def _writable(run_tmp):
    probe = run_tmp / ".write-probe"
    try:
        probe.write_bytes(b"ok")
        probe.unlink()
        return True
    except OSError:
        return False


def _wait_writable(run_tmp, timeout=300):
    """Wait until writes work again (the VM's storage limiter needs a
    quiet period after it trips): three clean probes, 5 s apart."""
    t0 = time.time()
    clean = 0
    while time.time() - t0 < timeout:
        if _writable(run_tmp):
            clean += 1
            if clean >= 3:
                return True
        else:
            clean = 0
        time.sleep(5)
    return False


# --------------------------------------------------------------------------
# Baseline
# --------------------------------------------------------------------------

def read_baseline(path):
    entries = set()
    counts = {}
    if not path or not Path(path).exists():
        return entries, counts
    for line in Path(path).read_text().splitlines():
        if line.startswith(("FAIL: ", "ERROR: ")):
            entries.add(line.strip())
        elif line.startswith("# counts:"):
            for part in line[len("# counts:"):].split():
                k, _, v = part.partition("=")
                if v.isdigit():
                    counts[k] = int(v)
    return entries, counts


def baseline_header(counts):
    import datetime
    import platform
    import subprocess
    try:
        sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=TESTS_DIR,
                             capture_output=True, text=True).stdout.strip()
    except Exception:
        sha = "unknown"
    return [
        "# museStopWatch esp32 host-test baseline — the known FAIL/ERROR set",
        "# for the standard flow (tests/run_host_tests.py). The runner diffs",
        "# every run against this file: entries appearing that are not listed",
        "# here fail the run; listed entries that pass are reported as FIXED.",
        "#",
        f"# derived_commit: {sha}",
        f"# derived_date: {datetime.date.today().isoformat()}",
        "# derived_how: python3 tests/run_host_tests.py --update-baseline",
        f"# environment: Python {platform.python_version()} on "
        f"{platform.system()} {platform.machine()}; no ESP-IDF "
        "managed_components in this VM, so classes whose setUpClass compiles",
        "#   against managed_components/espressif__cjson (and similar) error",
        "#   at setUpClass here. That is an environment property, not a code",
        "#   regression: on a machine with the components fetched, those",
        "#   classes run and this baseline must be re-derived.",
        "# counts: tests={tests} failures={failures} errors={errors} "
        "skipped={skipped}".format(**counts),
        "#",
        "# Re-derive this file ONLY when test code or the C harnesses change,",
        "# or when the environment changes (new system packages, components",
        "# fetched, Python/cc upgrades). Firmware-source-only changes must",
        "# NOT re-derive it — that is the whole point of recording it.",
        "",
    ]


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--jobs", type=int, default=1,
                    help="worker processes (default 1: the reliable pace "
                         "on this VM — its storage write limiter corrupts "
                         "parallel full runs, see the module docstring; "
                         "higher values pay off for --only subset runs)")
    ap.add_argument("--only", action="append", default=[],
                    help="only run test ids containing this substring "
                         "(repeatable)")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE),
                    help="baseline file to diff against")
    ap.add_argument("--update-baseline", action="store_true",
                    help="write the current FAIL/ERROR set as the baseline "
                         "instead of diffing")
    ap.add_argument("--tmpdir", default=os.environ.get("HOST_TEST_TMPDIR"),
                    help="disk-backed temp dir (default "
                         "~/workspace/.tmp-tests/host-tests-tmp)")
    ap.add_argument("--keep-tmp", action="store_true")
    ap.add_argument("--no-retry", action="store_true",
                    help="skip the serial re-run of EPERM-tainted units")
    ap.add_argument("--json", help="also write per-test rows as JSONL here")
    args = ap.parse_args()

    tmp_root = Path(args.tmpdir) if args.tmpdir else (
        Path.home() / "workspace" / ".tmp-tests" / "host-tests-tmp")
    run_tmp = Path(tempfile.mkdtemp(prefix=f"run-{os.getpid()}-",
                                     dir=tmp_root)) \
        if _ensure(tmp_root) else None
    if run_tmp is None:
        print("warning: cannot create disk-backed tmp dir; "
              "falling back to the default TMPDIR", file=sys.stderr)
        run_tmp = Path(tempfile.gettempdir())
    os.environ["TMPDIR"] = str(run_tmp)
    tempfile.tempdir = str(run_tmp)

    try:
        units = enumerate_units(args.only)
        n_cases = sum(len(u) for u in units)
        print(f"{len(units)} units, {n_cases} test cases, jobs={args.jobs}, "
              f"tmp={run_tmp}", flush=True)
        t0 = time.time()
        by_idx = {}
        rows_path = run_tmp / "rows.jsonl"
        rows_file = rows_path.open("a")

        def take(outcome):
            by_idx[outcome["idx"]] = outcome
            for r in outcome["rows"]:
                rows_file.write(json.dumps(r) + "\n")
            rows_file.flush()

        jobs = list(enumerate(units))
        if args.jobs <= 1:
            _worker_init(str(run_tmp))
            for job in jobs:
                take(_run_unit(job))
        else:
            with mp.Pool(args.jobs, initializer=_worker_init,
                         initargs=(str(run_tmp),)) as pool:
                for outcome in pool.imap_unordered(_run_unit, jobs):
                    take(outcome)
                    print(f"  ... {len(by_idx)}/{len(units)} units, "
                          f"{sum(o['testsRun'] for o in by_idx.values())} "
                          "tests run", flush=True)

        # EPERM retry: units whose results the VM's storage write limiter
        # corrupted get serial re-runs once writes work again (see the
        # module docstring), up to three rounds. Their fresh results
        # replace the tainted ones.
        tainted = [i for i, o in by_idx.items() if _eperm_tainted(o)]
        for round_no in range(1, 4):
            if not tainted or args.no_retry:
                break
            print(f"\n{len(tainted)} unit(s) hit the storage write limiter "
                  f"(EPERM); retry round {round_no}: waiting for writes "
                  "to recover, then re-running them serially...",
                  flush=True)
            rows_file.close()
            if not _wait_writable(run_tmp):
                print("writes did not recover within 300 s; giving up on "
                      "the retry — this run's new failures are not "
                      "trustworthy", flush=True)
                rows_file = rows_path.open("a")
                break
            rows_file = rows_path.open("a")
            _worker_init(str(run_tmp))
            for n, i in enumerate(sorted(tainted)):
                if n and n % 25 == 0 and not _writable(run_tmp):
                    if not _wait_writable(run_tmp):
                        break
                take(_run_unit((i, units[i])))
            still = [i for i in tainted if _eperm_tainted(by_idx[i])]
            print(f"retry round {round_no} done; "
                  f"{len(tainted) - len(still)} recovered, "
                  f"{len(still)} still tainted", flush=True)
            tainted = still
        rows_file.close()
        outcomes = [by_idx[i] for i in sorted(by_idx)]
        wall = time.time() - t0

        fatals = [o for o in outcomes if o.get("fatal")]
        if fatals:
            for o in fatals:
                print(f"WORKER DIED on {o['ids']}:\n{o['fatal']}",
                      file=sys.stderr)
            return 2

        rows = [r for o in outcomes for r in o["rows"]]
        tests_run = sum(o["testsRun"] for o in outcomes)
        eperm = sum(1 for r in rows
                    if "Operation not permitted" in r.get("trace", ""))
        if eperm:
            print(f"WARNING: {eperm} result(s) still carry 'Operation not "
                  "permitted' write errors after the retry — the VM's "
                  "storage rate limiter corrupted this run. Do not trust "
                  "new failures from it; re-run later or with --jobs 1.",
                  file=sys.stderr)
        bad = sorted({f"{r['status']}: {r['key']}" for r in rows
                      if r["status"] in ("FAIL", "ERROR")})
        counts = {
            "tests": tests_run,
            "failures": sum(1 for r in rows if r["status"] == "FAIL"),
            "errors": sum(1 for r in rows if r["status"] == "ERROR"),
            "skipped": sum(1 for r in rows if r["status"] == "skipped"),
        }
        print(f"\ntests={counts['tests']} failures={counts['failures']} "
              f"errors={counts['errors']} skipped={counts['skipped']} "
              f"wall={wall:.1f}s")

        if args.json:
            try:
                with open(args.json, "w") as f:
                    for r in sorted(rows, key=lambda r: r["key"]):
                        f.write(json.dumps(r) + "\n")
                # Writes on this VM have been seen to land as 0-byte files
                # under load without raising; verify the size.
                if rows and os.path.getsize(args.json) == 0:
                    print(f"warning: --json {args.json} landed as a 0-byte "
                          f"file (VM storage limiter); rows are in "
                          f"{rows_path}", file=sys.stderr)
            except OSError as e:
                print(f"warning: could not write --json {args.json}: {e}; "
                      f"rows are in {rows_path}", file=sys.stderr)

        if args.update_baseline:
            if eperm:
                # A baseline derived from a limiter-corrupted run would
                # poison every future diff. Refuse; re-run when quiet.
                print("refusing to write a baseline from an EPERM-tainted "
                      "run — re-run later", file=sys.stderr)
                return 2
            lines = baseline_header(counts) + bad + [""]
            Path(args.baseline).write_text("\n".join(lines))
            print(f"baseline written: {args.baseline} "
                  f"({len(bad)} known FAIL/ERROR entries)")
            return 0

        known, known_counts = read_baseline(Path(args.baseline))
        if args.only:   # a subset run diffs only its own scope
            known = {k for k in known
                     if any(s in k for s in args.only)}
        if not known and not args.only:
            print("no baseline found — run with --update-baseline first",
                  file=sys.stderr)
            return 2
        new = sorted(set(bad) - known)
        fixed = sorted(known - set(bad))
        for entry in new:
            print(f"NEW {entry}")
            for r in rows:
                if f"{r['status']}: {r['key']}" == entry and "trace" in r:
                    print("\n".join("    " + l
                                    for l in r["trace"].splitlines()[-25:]))
                    break
        for entry in fixed:
            print(f"FIXED {entry}")
        if known_counts and not args.only:
            print(f"(baseline counts: {known_counts}; "
                  f"current: {counts})")
        if new:
            print(f"\n{len(new)} NEW failure(s) vs baseline")
            return 1
        print("no new failures vs baseline"
              + (f"; {len(fixed)} baseline entr(ies) now pass" if fixed
                 else ""))
        return 0
    finally:
        if not args.keep_tmp and run_tmp.name.startswith("run-"):
            shutil.rmtree(run_tmp, ignore_errors=True)


def _ensure(path):
    try:
        path.mkdir(parents=True, exist_ok=True)
        return True
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main())
