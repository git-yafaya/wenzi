#!/usr/bin/env python3
"""实时观察 CLI 的 JSON 事件；只依赖 Python 标准库。"""

import json
import pathlib
import resource
import subprocess
import sys
import threading
import time
import wave


def check(wav_path, output_dir, command):
    with wave.open(str(wav_path)) as audio:
        assert (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (16000, 1, 2)
        duration = audio.getnframes() / audio.getframerate()
    output_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    running = draining = first_partial = last_final = idle = None
    finals, partials, previous, events = set(), 0, {}, []
    with (output_dir / "stderr.log").open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr,
                                   text=True, encoding="utf-8")
        timer = threading.Timer(max(60, duration * 3 + 20), process.kill)
        timer.start()
        try:
            for line in process.stdout:
                now = time.monotonic()
                event = json.loads(line)
                events.append({"elapsed_ms": (now - started) * 1000, "event": event})
                kind = event["type"]
                assert kind != "error", event
                if kind == "status":
                    if event["state"] == "running" and running is None:
                        running = now
                    elif event["state"] == "draining":
                        draining = now
                    elif event["state"] == "idle" and running is not None:
                        idle = now
                elif kind in ("partial", "final"):
                    key = (event["session_id"], event["utterance_id"])
                    assert event["text"] and key not in finals, event
                    if kind == "partial":
                        assert previous.get(key) != event["text"], event
                        previous[key] = event["text"]
                        partials += 1
                        if first_partial is None:
                            first_partial = now
                    else:
                        finals.add(key)
                        last_final = now
            result = process.wait()
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
    ended = time.monotonic()
    assert result == 0, f"CLI exit={result}; 查看 {output_dir / 'stderr.log'}"
    assert running and first_partial and finals and idle, "缺少 running/partial/final/idle"
    assert first_partial - running < duration, "首个 partial 在预计 WAV EOF 后出现"
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = usage.ru_utime + usage.ru_stime
    # 使用应用实际统计，区分推理处理与实时播放等待。
    statistics = next(json.loads(line.removeprefix("wenzi_statistics "))
                      for line in (output_dir / "stderr.log").read_text().splitlines()
                      if line.startswith("wenzi_statistics "))
    assert statistics["accepted_samples"] == round(duration * 16000), statistics
    assert statistics["overflow_samples"] == 0 and statistics["undelivered_samples"] == 0, statistics
    metrics = {
        "wav": str(wav_path), "command": command, "audio_seconds": duration,
        "wall_seconds": ended - started,
        "load_to_running_ms": (running - started) * 1000,
        "first_partial_after_running_ms": (first_partial - running) * 1000,
        "eof_final_after_draining_ms": (last_final - draining) * 1000
        if draining and last_final >= draining else None,
        "idle_after_draining_ms": (idle - draining) * 1000 if draining else None,
        "partial_count": partials, "final_count": len(finals),
        "cpu_seconds": cpu, "cpu_percent": 100 * cpu / (ended - started),
        # 包含模型加载和收尾；模型计算实时率以应用日志为准。
        "process_cpu_seconds_per_audio_second": cpu / duration,
        "peak_rss_kib": usage.ru_maxrss,
        "local_initialization_ms": statistics["local_initialization_seconds"] * 1000,
        "local_processing_rtf": statistics["local_processing_seconds"] /
        (statistics["local_samples"] / 16000),
        "application_statistics": statistics,
        "final_texts": [item["event"]["text"] for item in events
                        if item["event"]["type"] == "final"],
    }
    (output_dir / "events.json").write_text(json.dumps(events, ensure_ascii=False, indent=2) + "\n")
    (output_dir / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(metrics, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    if len(sys.argv) < 5 or sys.argv[3] != "--":
        sys.exit("用法: check_transcribe.py WAV OUTPUT_DIR -- WENZI_COMMAND [ARGS...]")
    check(pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]), sys.argv[4:])
