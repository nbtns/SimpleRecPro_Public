"""Exercise all eight assistant features through real MCP stdio and the C++ test host.

Usage: python tools/test_assistant_e2e.py --host <SimpleRecProTests.exe> --workdir <new-artifact-directory>
The host is headless and does not open microphone/speaker devices.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


class McpClient:
    def __init__(self, root, env):
        self.process = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("simplerecpro_mcp.py")), "--control-dir", str(root)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
        self.responses = queue.Queue()
        self.next_id = 0
        self.transcript = []
        def reader():
            for line in self.process.stdout:
                self.responses.put(json.loads(line))
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                    "clientInfo": {"name": "SimpleRecProE2E", "version": "1"}})
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def request(self, method, params):
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))
        self.process.stdin.flush()
        response = self.responses.get(timeout=35)
        self.transcript.append({"request": request, "response": response})
        assert response["id"] == self.next_id, response
        assert "error" not in response, response
        return response["result"]

    def call(self, name, arguments=None, expected_error=False):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        if expected_error:
            assert result.get("isError") is True, result
            return result
        assert result.get("isError") is not True, result
        return result.get("structuredContent") or json.loads(result["content"][0]["text"])

    def job(self, result):
        if "job_id" not in result:
            return result
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            result = self.call("get_job", {"job_id": result["job_id"]})
            if result["status"] == "completed":
                return result["result"]
            assert result["status"] != "failed", result
            time.sleep(0.05)
        raise AssertionError("Background job did not finish")

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


def track(context):
    return next(item for item in context["tracks"] if item["track_id"] == "e2e-vocal")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    options = parser.parse_args()
    workdir = options.workdir.resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    control = workdir / "control"
    env = dict(os.environ, SIMPLE_REC_CONTROL_DIR=str(control))
    host_log = (workdir / "host.log").open("wb")
    host = subprocess.Popen([str(options.host.resolve()), "--assistant-host", str(workdir)],
                            stdout=host_log, stderr=subprocess.STDOUT, env=env,
                            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
    client = None
    completed = []
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                manifest = json.loads((control / "manifest.json").read_text(encoding="utf-8-sig"))
                if manifest.get("pid") == host.pid:
                    break
            except (OSError, ValueError):
                pass
            assert host.poll() is None, "C++ host exited; inspect host.log"
            assert time.monotonic() < deadline, "C++ host did not publish its manifest"
            time.sleep(0.05)
        client = McpClient(control, env)
        assert len(client.request("tools/list", {})["tools"]) == 17
        initial = client.call("get_context")
        baseline = track(initial)["settings"]
        context = client.call("select_target", {"track_id": "e2e-vocal", "clip_id": "e2e-clip",
                                               "start_seconds": 0.25, "end_seconds": 1.5})
        assert context["selected_track_id"] == "e2e-vocal"
        assert context["selected_clip_id"] == "e2e-clip"
        assert context["selection"]["start_seconds"] == 0.25
        completed.append("target selection")

        client.call("adjust_sound", {"gain_db": -3, "brightness": 0.35, "ambience": 0.15,
                                    "stability": 0.2, "noise_reduction": 0.1, "de_ess": 0.25})
        adjusted = track(client.call("get_context"))["settings"]
        assert abs(adjusted["brightness"] - 0.35) < 1e-5
        assert abs(adjusted["volume"] / baseline["volume"] - math.pow(10, -3 / 20)) < 1e-5
        completed.append("sound adjustment")

        client.call("undo_edit")
        assert track(client.call("get_context"))["settings"] == baseline
        client.call("redo_edit")
        assert track(client.call("get_context"))["settings"] == adjusted
        client.call("scale_last_edit", {"factor": 0.5})
        scaled = track(client.call("get_context"))["settings"]
        assert abs(scaled["brightness"] - (baseline["brightness"] + adjusted["brightness"]) / 2) < 1e-5
        completed.append("undo redo and scale")

        client.call("adjust_sound", {"scope": "selection", "gain_db": -2, "brightness": 0.2, "ambience": 0.1})
        region = track(client.call("get_context"))["automation_regions"][-1]
        assert region["start_seconds"] == 0.25 and region["end_seconds"] == 1.5
        assert region["gain_db"] == -2
        client.call("adjust_sound", {"scope": "selection", "stability": 0.3}, expected_error=True)
        client.call("add_effect", {"type": "radio", "amount": 0.4, "wet": 0.7, "fade_seconds": 0.02})
        effects = track(client.call("get_context"))["special_fx_regions"]
        assert len(effects) == 1 and abs(effects[0]["wet"] - 0.7) < 1e-5
        completed.append("range automation and effects")

        analysis = client.job(client.call("analyse_audio", {"scope": "selection"}))
        assert analysis["valid"] is True and math.isfinite(analysis["rms_db"])
        assert 0 < analysis["analysed_seconds"] <= 1.26, analysis
        completed.append("audio analysis")

        variants = client.call("create_variants")["variants"]
        assert {item["variant_id"] for item in variants} == {"natural", "clear", "wide"}
        for variant_id in ("natural", "wide", "clear"):
            client.call("choose_variant", {"variant_id": variant_id})
        chosen = track(client.call("get_context"))["settings"]
        clear = next(item for item in variants if item["variant_id"] == "clear")["settings"]
        assert abs(chosen["brightness"] - clear["brightness"]) < 1e-5
        completed.append("three variants")

        client.call("save_preference", {"name": "E2E 声の好み"})
        assert "E2E 声の好み" in {item["name"] for item in client.call("list_preferences")["preferences"]}
        client.call("adjust_sound", {"brightness": -0.8})
        client.call("apply_preference", {"name": "E2E 声の好み"})
        assert abs(track(client.call("get_context"))["settings"]["brightness"] - chosen["brightness"]) < 1e-5
        reference = client.job(client.call("match_reference", {"file_path": str(workdir / "reference.wav"), "strength": 0.5}))
        assert reference and "error" not in reference
        completed.append("reference and saved preference")

        before = client.job(client.call("compare", {"mode": "before"}))
        assert before and client.call("get_context")["preview_playing"] is True
        assert before["measurement"] == "processed_export_rms"
        assert 0 < before["before_gain"] <= 1 and 0 < before["after_gain"] <= 1
        corrected_before = before["before_rms_db"] + 20 * math.log10(before["before_gain"])
        corrected_after = before["after_rms_db"] + 20 * math.log10(before["after_gain"])
        assert abs(corrected_before - corrected_after) < 0.001
        assert Path(before["before_file"]).stat().st_size > 100
        assert Path(before["after_file"]).stat().st_size > 100
        after = client.job(client.call("compare", {"mode": "after"}))
        assert after and client.call("get_context")["preview_playing"] is True
        client.call("compare", {"mode": "stop"})
        assert client.call("get_context")["preview_playing"] is False
        completed.append("before after comparison")

        client.call("transport", {"command": "seek", "position_seconds": 0.5})
        assert abs(client.call("get_context")["position_seconds"] - 0.5) < 1e-5
        client.call("transport", {"command": "play"})
        client.call("transport", {"command": "pause"})
        client.call("transport", {"command": "loop_selection"})
        client.call("transport", {"command": "stop"})
        (workdir / "result.json").write_text(json.dumps({"ok": True, "features": completed}, indent=2), encoding="utf-8")
        print("PASS: " + ", ".join(completed))
    finally:
        if host.poll() is not None:
            print(f"C++ host exited: {host.returncode} (0x{host.returncode & 0xffffffff:08x})", file=sys.stderr)
        if client:
            (workdir / "mcp-transcript.json").write_text(json.dumps(client.transcript, ensure_ascii=False, indent=2), encoding="utf-8")
            client.close()
        if host.poll() is None:
            host.terminate()  # Only the dedicated child test host created above.
            host.wait(timeout=10)
        host_log.close()


if __name__ == "__main__":
    main()
