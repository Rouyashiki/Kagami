"""Check module initialization and late-load mount requests with a fake controller."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


CONTROLLER = r"""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
with open(os.environ["KAGAMI_TEST_CALLS"], "a", encoding="utf-8") as output:
    output.write(json.dumps([args, os.environ.get("KAGAMI_ALLOW_DAEMON_START")]) + "\n")
if args[0] == "prepare":
    sys.exit(int(os.environ.get("KAGAMI_TEST_PREPARE_EXIT", "0")))
if args[:2] == ["config", "gen"]:
    Path(args[3]).write_text("default config\n", encoding="utf-8")
"""


def main():
    source = Path(__file__).resolve().parents[1] / "module"
    with tempfile.TemporaryDirectory(prefix="kagami-module-scripts-") as temporary:
        root = Path(temporary)
        module = root / "module with spaces"
        state = root / "state"
        calls = root / "calls.jsonl"
        module.mkdir()
        for name in ("late-load.sh", "post-fs-data.sh", "metamount.sh"):
            script = (source / name).read_text(encoding="utf-8")
            script = script.replace('BASE_DIR="/data/adb/kagami"',
                                    "BASE_DIR=" + shlex.quote(str(state)))
            script = script.replace("/data/adb/ksud", str(root / "ksud"))
            (module / name).write_text(script, encoding="utf-8")
        controller = module / "kagamid"
        controller.write_text(CONTROLLER, encoding="utf-8")
        controller.chmod(0o755)
        shutil.copyfile(controller, root / "ksud")
        (root / "ksud").chmod(0o755)
        env = dict(os.environ, KAGAMI_TEST_CALLS=str(calls))
        env.pop("KAGAMI_ALLOW_DAEMON_START", None)
        env.pop("KAGAMI_TEST_PREPARE_EXIT", None)
        env.pop("KSU_LATE_LOAD", None)

        def run(name, **changes):
            calls.write_text("", encoding="utf-8")
            result = subprocess.run(["sh", str(module / name)], env=env | changes,
                                    capture_output=True, text=True)
            recorded = [json.loads(line) for line in calls.read_text().splitlines()]
            return result.returncode, recorded

        status, recorded = run("late-load.sh", KSU_LATE_LOAD="1")
        assert status == 0
        assert recorded == [
            [["prepare", str(module)], None],
            [["config", "gen", "-o", str(state / "config.json")], None],
        ]

        config = state / "config.json"
        config.write_text("saved config\n", encoding="utf-8")
        for name in ("post-fs-data.sh", "late-load.sh"):
            status, recorded = run(name)
            assert status == 0 and recorded == [[["prepare", str(module)], None]]
            assert config.read_text() == "saved config\n"
            status, recorded = run(name, KAGAMI_TEST_PREPARE_EXIT="1")
            assert status == 1 and recorded == [[["prepare", str(module)], None]]

        for value in (None, "0", "1", "true"):
            changes = {} if value is None else {"KSU_LATE_LOAD": value}
            status, recorded = run("metamount.sh", **changes)
            assert status == 0
            arguments = ["module", "mount-all"]
            if value == "1":
                arguments.append("--late-load")
            assert recorded == [
                [arguments, "1"],
                [["kernel", "notify-module-mounted"], None],
            ]
    print("PASS: initialization, saved config, preparation failure and late-load mount arguments")


if __name__ == "__main__":
    main()
