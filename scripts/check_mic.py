#!/usr/bin/env python3
"""真实 ALSA 输入进入完整 CLI 后，用 SIGINT 检查收尾。"""
import json
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time


def check(output, seconds, command):
    output.mkdir(parents=True, exist_ok=True)
    with (output / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr, text=True)
        watchdog = threading.Timer(seconds + 30, process.kill)
        watchdog.start()
        stop = None
        events = []
        started = time.monotonic()
        try:
            for line in process.stdout:
                event = json.loads(line)
                events.append(event)
                assert event["type"] != "error", event
                if event["type"] == "status" and event["state"] == "running" and stop is None:
                    stop = threading.Timer(seconds, lambda: process.send_signal(signal.SIGINT))
                    stop.start()
            assert process.wait() == 0, "CLI 非正常退出"
        finally:
            watchdog.cancel()
            if stop:
                stop.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
    assert stop and events[-1]["state"] == "idle", "没有运行或没有收尾"
    statistics = next(json.loads(line.removeprefix("wenzi_statistics "))
                      for line in (output / "stderr.log").read_text().splitlines()
                      if line.startswith("wenzi_statistics "))
    assert statistics["accepted_samples"] > 0 and statistics["overflow_samples"] == 0, statistics
    report = {"command": command, "wall_seconds": time.monotonic() - started,
              "capture_seconds": seconds, "exit_code": 0, "statistics": statistics,
              "events": events}
    (output / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    if len(sys.argv) < 5 or sys.argv[3] != "--":
        sys.exit("用法: check_mic.py OUTPUT_DIR SECONDS -- WENZI_COMMAND [ARGS...]")
    check(Path(sys.argv[1]), float(sys.argv[2]), sys.argv[4:])
