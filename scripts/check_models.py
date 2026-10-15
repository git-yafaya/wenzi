#!/usr/bin/env python3
"""用独立 CLI 进程验证损坏模型和填错模型位置的初始化失败。"""
import argparse
import json
from pathlib import Path
import resource
import subprocess
import tempfile


def check(args):
    args.output_dir.mkdir(parents=True, exist_ok=True)
    common = [str(args.wenzi.resolve()), "transcribe", "--wav", str(args.wav.resolve()),
              "--model-dir", str(args.model_dir.resolve())]
    records = []
    with tempfile.TemporaryDirectory(prefix="model-errors-", dir=args.output_dir) as temporary:
        bad = Path(temporary) / "bad.onnx"
        bad.write_text("This is not an ONNX model.\n")
        encoder = args.model_dir.resolve() / "encoder-epoch-99-avg-1.int8.onnx"
        decoder = args.model_dir.resolve() / "decoder-epoch-99-avg-1.onnx"
        cases = [("broken_punctuation", "--punctuation", bad, True),
                 ("broken_encoder", "--encoder", bad, False),
                 ("punctuation_with_encoder", "--punctuation", encoder, True),
                 ("encoder_with_decoder", "--encoder", decoder, False)]
        for name, option, model, keeps_text in cases:
            command = common + [option, str(model)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            (args.output_dir / f"{name}.stdout.jsonl").write_text(result.stdout)
            (args.output_dir / f"{name}.stderr.log").write_text(result.stderr)
            events = [json.loads(line) for line in result.stdout.splitlines()]
            errors = [e for e in events if e["type"] == "error"]
            finals = [e for e in events if e["type"] == "final"]
            assert result.returncode == 1, f"{name}: exit={result.returncode}"
            assert errors and all(e["code"] > 0 for e in errors), f"{name}: 没有可控 error"
            assert bool(finals) == keeps_text, f"{name}: 标点失败应保留原文，本地 ASR 失败不生成文本"
            records.append({"case": name, "exit_code": result.returncode,
                            "error_codes": [e["code"] for e in errors],
                            "finals": [e["text"] for e in finals]})
    (args.output_dir / "summary.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(records, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wenzi", type=Path, default=root / "build/wenzi")
    parser.add_argument("--wav", type=Path, default=root / "samples/zh.wav")
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=root / "build/acceptance/model-errors")
    check(parser.parse_args())
