#!/usr/bin/env python3
"""Exercise the real batch script with a fake xcrun; no Apple SDK or device is used."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


SCRIPT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "scripts/ios-runs.sh"
MOCK_XCRUN = textwrap.dedent("""\
    #!/usr/bin/env python3
    import json, os, shutil, sys
    from pathlib import Path
    args = sys.argv[1:]
    data = Path(os.environ['MOCK_DATA'])
    def event(text):
        with open(os.environ['MOCK_EVENTS'], 'a') as stream:
            stream.write(text + '\\n')
    if args[:4] == ['devicectl', 'device', 'process', 'launch'] or args[:2] == ['simctl', 'launch']:
        cli = args[args.index('--bmoe') + 1:]
        name = 'plan' if '--plan' in cli else Path(cli[cli.index('--csv') + 1]).stem
        event('launch ' + name)
        if name != 'plan' and name != os.environ.get('MOCK_MISSING_CSV'):
            (data / 'Documents' / (name + '.csv')).write_text(
                'step,dense_resident_frac,majflt\\n1,0.75,2\\n')
        print('generation: fixture for ' + name)
        sys.exit(json.loads(os.environ['MOCK_STATUSES']).get(name, 0))
    if args[:4] == ['devicectl', 'device', 'copy', 'from']:
        source = args[args.index('--source') + 1]
        event('fetch ' + Path(source).name)
        if not (data / source).is_file():
            sys.exit(11)
        shutil.copyfile(data / source, args[args.index('--destination') + 1])
        sys.exit(0)
    if args[:2] == ['simctl', 'get_app_container']:
        event('container')
        print(data)
        sys.exit(0)
    sys.exit('unexpected xcrun invocation: ' + repr(args))
""")


class IosRunsTest(unittest.TestCase):
    def run_batch(self, target, runs, statuses=None, missing_csv=""):
        temporary = tempfile.TemporaryDirectory(prefix="bmoe-ios-runs-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "bin").mkdir()
        (root / "data/Documents").mkdir(parents=True)
        xcrun = root / "bin/xcrun"
        xcrun.write_text(MOCK_XCRUN)
        xcrun.chmod(0o755)
        output = root / "outputs"
        env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"],
                   MOCK_DATA=str(root / "data"), MOCK_EVENTS=str(root / "events"),
                   MOCK_STATUSES=json.dumps(statuses or {}), MOCK_MISSING_CSV=missing_csv,
                   BMOE_IOS_DEVICE="fixture-device", OUT=str(output), RUNS=runs)
        result = subprocess.run(["bash", str(SCRIPT), target, "fixture.gguf"], env=env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        events = (root / "events").read_text().splitlines()
        return result, output, events

    def test_all_success(self):
        for target in ("device", "sim"):
            with self.subTest(target=target):
                result, output, events = self.run_batch(target, "plan auto mmap t2 t4")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([x for x in events if x.startswith("launch ")],
                                 ["launch " + name for name in ("plan", "auto", "mmap", "t2", "t4")])
                self.assertEqual((output / "summary.txt").read_text().count("(exit 0)"), 5)
                for name in ("auto", "mmap", "t2", "t4"):
                    self.assertTrue((output / (name + ".csv")).is_file())

    def test_failures_preserve_exact_status_and_continue(self):
        for target in ("device", "sim"):
            with self.subTest(target=target):
                result, output, events = self.run_batch(target, "plan auto mmap t2 t4", {"auto": 7, "t2": 23})
                self.assertEqual(result.returncode, 1)
                self.assertEqual([x for x in events if x.startswith("launch ")],
                                 ["launch " + name for name in ("plan", "auto", "mmap", "t2", "t4")])
                summary = (output / "summary.txt").read_text()
                for name, status in (("plan", 0), ("auto", 7), ("mmap", 0), ("t2", 23), ("t4", 0)):
                    self.assertIn("== %s (exit %d)" % (name, status), summary)
                    self.assertTrue((output / (name + ".txt")).read_text().endswith("exit %d\n" % status))
                for name in ("auto", "mmap", "t2", "t4"):
                    self.assertTrue((output / (name + ".csv")).is_file())

    def test_plan_only_needs_no_csv(self):
        for target in ("device", "sim"):
            with self.subTest(target=target):
                result, output, _ = self.run_batch(target, "plan")
                self.assertEqual(result.returncode, 0)
                self.assertFalse((output / "plan.csv").exists())
                self.assertIn("== plan (exit 0)", (output / "summary.txt").read_text())

    def test_failed_plan_is_not_hidden(self):
        result, output, _ = self.run_batch("device", "plan", {"plan": 41})
        self.assertEqual(result.returncode, 1)
        self.assertIn("== plan (exit 41)", (output / "summary.txt").read_text())

    def test_failed_fetch_does_not_stop_later_cases(self):
        result, output, events = self.run_batch("device", "auto mmap", {"auto": 9}, missing_csv="auto")
        self.assertEqual(result.returncode, 1)
        self.assertIn("fetch auto.csv", events)
        self.assertIn("fetch mmap.csv", events)
        self.assertTrue((output / "mmap.csv").is_file())
        self.assertIn("== mmap (exit 0)", (output / "summary.txt").read_text())


if __name__ == "__main__":
    unittest.main()
