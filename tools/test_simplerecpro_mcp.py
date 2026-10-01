"""Protocol and local IPC contract tests (no audio device required)."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import simplerecpro_mcp as mcp


class FakeBridge:
    def call(self, action, args):
        return {"action": action, "args": args}


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.server = mcp.Server(FakeBridge())
        self.init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "test", "version": "1"}}}

    def call(self, method, params=None):
        return self.server.handle({"jsonrpc": "2.0", "id": 2, "method": method, "params": params or {}})

    def test_initialize_and_list(self):
        self.assertEqual(self.call("tools/list")["error"]["code"], -32002)
        self.assertEqual(self.server.handle(self.init)["result"]["protocolVersion"], "2025-11-25")
        names = {tool["name"] for tool in self.call("tools/list")["result"]["tools"]}
        self.assertEqual(len(names), 17)
        self.assertTrue({"get_context", "compare", "get_job", "match_reference"}.issubset(names))
        self.assertIsNone(self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))

    def test_version_negotiation(self):
        self.init["params"]["protocolVersion"] = "2099-01-01"
        self.assertEqual(self.server.handle(self.init)["result"]["protocolVersion"], mcp.PROTOCOLS[0])

    def test_call_and_validation(self):
        self.server.handle(self.init)
        response = self.call("tools/call", {"name": "adjust_sound", "arguments": {"gain_db": 3}})
        self.assertFalse(response["result"]["isError"])
        self.assertEqual(response["result"]["structuredContent"]["args"]["gain_db"], 3)
        for args in ({"gain_db": 999}, {"gain_db": 10 ** 1000}, {"gain_db": float("nan")}, {"gain_db": True},
                     {"scope": "selection", "de_ess": 0.5}, {"made_up": 0}):
            response = self.call("tools/call", {"name": "adjust_sound", "arguments": args})
            self.assertTrue(response["result"]["isError"])
        self.assertEqual(self.call("tools/call", {"name": "absent"})["error"]["code"], -32602)

    def test_stdio_clean_utf8(self):
        messages = [self.init, {"jsonrpc": "2.0", "method": "notifications/initialized"},
                    {"jsonrpc": "2.0", "id": 3, "method": "tools/list"}]
        process = subprocess.run([sys.executable, str(Path(mcp.__file__))],
                                 input="".join(json.dumps(x) + "\n" for x in messages).encode(),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual([x["id"] for x in replies], [1, 3])
        self.assertEqual(len(replies[1]["result"]["tools"]), 17)

    def test_stdio_bad_messages_do_not_break_following_request(self):
        good = json.dumps({"jsonrpc": "2.0", "id": 9, "method": "ping"}).encode() + b"\n"
        messages = b"not json\n" + b"x" * (mcp.MAX_BYTES + 500) + b"\n" + good
        process = subprocess.run([sys.executable, str(Path(mcp.__file__))], input=messages,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual([reply.get("error", {}).get("code") for reply in replies], [-32700, -32600, None])
        self.assertEqual(replies[-1]["id"], 9)


class FileBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "session" / "requests").mkdir(parents=True)
        (self.root / "session" / "responses").mkdir()
        self.manifest = {"protocol": 1, "session": "session", "token": "test-token", "pid": os.getpid(),
                         "request_dir": "session/requests", "response_dir": "session/responses"}
        self.write_manifest()
        self.bridge = mcp.FileBridge(self.root, timeout=1)

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self):
        temporary = self.root / "manifest.tmp"
        temporary.write_text(json.dumps(self.manifest), encoding="utf-8")
        os.replace(temporary, self.root / "manifest.json")

    def responder(self, mode="success"):
        def run():
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                files = list((self.root / "session" / "requests").glob("*.json"))
                if not files:
                    time.sleep(0.005)
                    continue
                request = json.loads(files[0].read_text(encoding="utf-8"))
                self.assertEqual(request["token"], "test-token")
                self.assertGreater(request["expires_at_ms"], time.time() * 1000)
                if mode == "restart":
                    self.manifest["session"] = "other-session"
                    self.write_manifest()
                    return
                response = {"id": request["id"], "session": request["session"], "ok": True,
                            "result": {"selected": "声"}}
                if mode == "mismatch":
                    response["session"] = "wrong"
                if mode == "error":
                    response.update(ok=False, error="No selected track")
                destination = self.root / "session" / "responses" / files[0].name
                temporary = destination.with_suffix(".tmp")
                temporary.write_text(json.dumps(response), encoding="utf-8")
                os.replace(temporary, destination)
                return
        worker = threading.Thread(target=run)
        worker.start()
        return worker

    def test_round_trip_and_cleanup(self):
        worker = self.responder()
        self.assertEqual(self.bridge.call("get_context", {}), {"selected": "声"})
        worker.join()
        self.assertEqual(list((self.root / "session" / "requests").iterdir()), [])
        self.assertEqual(list((self.root / "session" / "responses").iterdir()), [])

    def test_timeout_does_not_retry(self):
        self.bridge.timeout = 0.1
        with self.assertRaisesRegex(mcp.BridgeError, "may have completed"):
            self.bridge.call("adjust_sound", {"gain_db": 1})
        self.assertEqual(list((self.root / "session" / "requests").iterdir()), [])

    def test_session_change_and_response_mismatch(self):
        for mode, message in (("restart", "session changed"), ("mismatch", "session mismatch"),
                              ("error", "No selected track")):
            self.manifest["session"] = "session"
            self.write_manifest()
            worker = self.responder(mode)
            with self.assertRaisesRegex(mcp.BridgeError, message):
                self.bridge.call("get_context", {})
            worker.join()

    def test_dead_process_and_traversal(self):
        with patch.object(mcp, "process_alive", return_value=False):
            with self.assertRaisesRegex(mcp.BridgeError, "stopped"):
                self.bridge.call("get_context", {})
        self.manifest["request_dir"] = "../"
        self.write_manifest()
        with self.assertRaisesRegex(mcp.BridgeError, "outside"):
            self.bridge.call("get_context", {})

    def test_environment_override(self):
        with patch.dict(os.environ, {"SIMPLE_REC_CONTROL_DIR": str(self.root)}):
            self.assertEqual(mcp.default_control_dir(), self.root.resolve())

    def test_manifest_atomic_replace_read_race(self):
        original = mcp.read_json
        calls = 0
        def transient(path, limit=mcp.MAX_BYTES):
            nonlocal calls
            calls += 1
            if calls <= 2:
                raise PermissionError("atomic replacement in progress")
            return original(path, limit)
        with patch.object(mcp, "read_json", side_effect=transient):
            self.assertEqual(self.bridge.manifest()["session"], "session")
        self.assertEqual(calls, 3)


if __name__ == "__main__":
    unittest.main()
