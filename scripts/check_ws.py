#!/usr/bin/env python3
"""用真实模型验收 serve 的 WebSocket 接线，只使用标准库。"""
import argparse
import base64
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import time
import wave


class WebSocket:
    def __init__(self, sock, stream, events, timeout, allow_errors):
        self.sock, self.stream, self.log = sock, stream, events
        self.timeout, self.allow_errors = timeout, allow_errors
        self.events = []
        self.commands = []
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((
            f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{sock.getpeername()[1]}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: wenzi\r\n\r\n"
        ).encode())
        assert stream.readline().startswith(b"HTTP/1.1 101 "), "握手未升级"
        headers = {}
        while (line := stream.readline()) != b"\r\n":
            assert line, "握手连接断开"
            name, _, value = line.partition(b":")
            headers[name.lower()] = value.strip()
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest())
        assert headers.get(b"sec-websocket-accept") == accept, "握手签名不符"
        assert headers.get(b"sec-websocket-protocol") == b"wenzi", "子协议不符"

    def frame(self, opcode, payload):
        # 本检查只发送短命令和 pong，客户端帧必须带掩码。
        assert len(payload) < 126
        mask = os.urandom(4)
        masked = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        self.sock.sendall(bytes((0x80 | opcode, 0x80 | len(payload))) + mask + masked)

    def send(self, cmd, **fields):
        command = {"cmd": cmd, **fields}
        self.commands.append(command)
        self.frame(1, json.dumps(command).encode())

    def read(self, count, deadline):
        remaining = deadline - time.monotonic()
        assert remaining > 0, "等待事件超时"
        self.sock.settimeout(remaining)
        data = self.stream.read(count)
        assert len(data) == count, "WebSocket 提前关闭"
        return data

    def expect(self, predicate, label):
        deadline = time.monotonic() + self.timeout
        while True:
            first, second = self.read(2, deadline)
            assert first & 0x80 and not first & 0x70, "收到不支持的分片或压缩帧"
            assert not second & 0x80, "服务端帧不应带掩码"
            length = second & 0x7f
            if length == 126:
                length = struct.unpack("!H", self.read(2, deadline))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.read(8, deadline))[0]
            assert length <= 65536, "事件超过服务端消息上限"
            payload = self.read(length, deadline)
            opcode = first & 0x0f
            if opcode == 9:
                self.frame(10, payload)
                continue
            assert opcode == 1, f"{label}: 收到非文本帧 {opcode}"
            event = json.loads(payload)
            assert {"type", "engine", "state", "session_id", "utterance_id", "timestamp"} <= event.keys(), "事件缺少公共字段"
            self.events.append(event)
            self.log.write(json.dumps(event, ensure_ascii=False) + "\n")
            self.log.flush()
            assert self.allow_errors or event["type"] != "error", f"{label}: {event}"
            if predicate(event):
                return event

    def command(self, cmd, state, engine="local", **fields):
        if cmd == "switch_engine":
            fields["engine"] = engine
        self.send(cmd, **fields)
        return self.expect(lambda e: e["type"] == "status" and
                           e["state"] == state and e["engine"] == engine,
                           f"{cmd} -> {engine}/{state}")

    def partial(self, session, started, duration):
        event = self.expect(lambda e: e["type"] == "partial", "实时 partial")
        elapsed = time.monotonic() - started
        assert event["session_id"] == session and event["state"] == "running"
        assert event.get("text") and elapsed < duration, "未在音频结束前收到非空 partial"
        return elapsed


@contextlib.contextmanager
def serve(args, name, report, expected_exit):
    paths = {key: args.output_dir / f"{name}.{suffix}" for key, suffix in (
        ("stdout", "stdout.jsonl"), ("stderr", "stderr.log"), ("ws", "ws.jsonl"))}
    env = os.environ.copy()
    # 清除真实凭据，并固定本地地址；缺凭据测试不会访问公网。
    for key in ("WENZI_IFLYTEK_APP_ID", "WENZI_IFLYTEK_API_KEY", "WENZI_PUNCTUATION_MODEL"):
        env.pop(key, None)
    command = [str(args.wenzi), "serve", "--wav", str(args.wav),
               "--model-dir", str(args.model_dir), "--port", "0",
               "--iflytek-url", "wss://127.0.0.1:1/"]
    report["command"] = command
    with paths["stdout"].open("wb") as stdout, paths["stderr"].open("wb") as stderr, \
            paths["ws"].open("w", encoding="utf-8") as events:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env)
        try:
            deadline = time.monotonic() + args.timeout
            while True:
                match = re.search(r"WebSocket 服务：127\.0\.0\.1:(\d+)",
                                  paths["stderr"].read_text(errors="replace"))
                if match:
                    port = int(match.group(1))
                    break
                assert process.poll() is None, "serve 在监听前退出，查看 stderr.log"
                assert time.monotonic() < deadline, "等待监听端口超时"
                time.sleep(0.05)
            report["port"] = port
            with socket.create_connection(("127.0.0.1", port), args.timeout) as sock, \
                    sock.makefile("rb") as stream:
                client = WebSocket(sock, stream, events, args.timeout, expected_exit != 0)
                client.pid = process.pid
                yield client
                status = Path(f"/proc/{process.pid}/status").read_text()
                peak = re.search(r"VmHWM:\s+(\d+) kB", status)
                report["peak_rss_kib"] = int(peak.group(1)) if peak else None
                # stop 后服务仍存活，最后由信号正常退出。
                process.send_signal(signal.SIGINT)
                report["exit_code"] = process.wait(timeout=args.timeout)
                assert report["exit_code"] == expected_exit, f"退出码应为 {expected_exit}"
                report["commands"] = client.commands
                report["events"] = len(client.events)
                report["partials"] = sum(e["type"] == "partial" for e in client.events)
                finals = [(e["session_id"], e["utterance_id"]) for e in client.events
                          if e["type"] == "final"]
                assert len(finals) == len(set(finals)), "同一句重复 final"
                report["finals"] = len(finals)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    errors = [e for line in paths["stdout"].read_text().splitlines()
              if (e := json.loads(line))["type"] == "error"]
    if expected_exit == 0:
        assert not errors, "正常路径出现 error，查看 stdout.jsonl"
    else:
        assert errors and all(abs(e["code"]) == errno.ENOKEY for e in errors), "缺凭据路径出现其他错误"


def local(client, duration, report):
    assert client.command("status", "idle")["session_id"] == 0
    session = client.command("start", "running")["session_id"]
    started = time.monotonic()
    assert session == 1
    assert client.command("start", "running")["session_id"] == session
    for _ in range(2):
        assert client.command("switch_engine", "running", engine="local")["session_id"] == session
    report["partial_seconds"] = client.partial(session, started, duration)
    mark = len(client.events)
    assert client.command("stop", "idle")["session_id"] == session
    assert any(e["type"] == "final" and e.get("reason") == "stop" and e.get("text")
               for e in client.events[mark:]), "stop 未确认尾句"
    assert client.command("status", "idle")["session_id"] == session
    assert client.command("stop", "idle")["session_id"] == session


def recovery(client, duration, report):
    client.command("status", "idle")
    session = client.command("start", "running")["session_id"]
    client.partial(session, time.monotonic(), duration)
    client.send("switch_engine", engine="cloud")
    error = client.expect(lambda e: e["type"] == "error" and e["engine"] == "cloud",
                          "cloud 缺凭据")
    assert abs(error["code"]) == errno.ENOKEY and "WENZI_IFLYTEK" in error["message"]
    client.expect(lambda e: e["type"] == "status" and e["state"] == "idle" and
                  e["engine"] == "cloud", "cloud 失败后 idle")
    assert client.command("status", "idle", engine="cloud")["session_id"] == session
    client.command("switch_engine", "idle", engine="local")
    assert client.command("start", "running")["session_id"] == session + 1
    report["recovered_partial_seconds"] = client.partial(session + 1, time.monotonic(), duration)
    assert client.command("stop", "idle")["session_id"] == session + 1
    client.command("status", "idle")
    errors = [e for e in client.events if e["type"] == "error"]
    assert errors and all(abs(e["code"]) == errno.ENOKEY for e in errors)


def sustained(client, duration, report, cycles):
    # 同一服务连续完成多次真实 WAV，核对 EOF 后仍可再次开始。
    started = time.monotonic()
    client.command("status", "idle")
    report["cycles"] = []
    for index in range(cycles):
        mark = len(client.events)
        session = client.command("start", "running")["session_id"]
        assert session == index + 1
        partial = client.partial(session, time.monotonic(), duration)
        client.expect(lambda e: e["type"] == "status" and e["state"] == "idle",
                      "EOF 后 idle")
        finals = [e for e in client.events[mark:] if e["type"] == "final"]
        assert finals and finals[-1]["reason"] == "eof", "EOF 尾句没有确认"
        assert client.command("status", "idle")["session_id"] == session
        status = Path(f"/proc/{client.pid}/status").read_text()
        rss = re.search(r"VmRSS:\s+(\d+) kB", status)
        report["cycles"].append({"session_id": session, "first_partial_seconds": partial,
                                  "finals": len(finals),
                                  "rss_after_eof_kib": int(rss.group(1)) if rss else None})
    report["wall_seconds"] = time.monotonic() - started


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wenzi", type=Path, default=root / "build/wenzi")
    parser.add_argument("--wav", type=Path, default=root / "samples/mixed.wav")
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=root / "build/acceptance/ws")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--cycles", type=int, default=6)
    args = parser.parse_args()
    for key in ("wenzi", "wav", "model_dir", "output_dir"):
        setattr(args, key, getattr(args, key).resolve())
    if args.timeout <= 0 or args.cycles <= 0:
        parser.error("timeout 和 cycles 必须为正数")
    for path in (args.wenzi, args.wav, args.model_dir):
        if not path.exists():
            parser.error(f"文件或目录不存在: {path}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.wav), "rb") as audio:
        duration = audio.getnframes() / audio.getframerate()
    report = {"wav_seconds": duration, "cases": {}}
    try:
        for name, check, exit_code in (("local", local, 0), ("recovery", recovery, 1)):
            case = report["cases"][name] = {}
            with serve(args, name, case, exit_code) as client:
                check(client, duration, case)
        case = report["cases"]["sustained"] = {}
        with serve(args, "sustained", case, 0) as client:
            sustained(client, duration, case, args.cycles)
        report["result"] = "PASS"
    except Exception as error:
        report["result"] = "FAIL"
        report["error"] = f"{type(error).__name__}: {error}"
    (args.output_dir / "summary.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
