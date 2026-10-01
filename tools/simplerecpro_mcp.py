#!/usr/bin/env python3
"""SimpleRecPro MCP stdio server. Python 3.10+, standard library only.

The file bridge is for trusted processes belonging to the same OS user.
It is not a security boundary against that user or an administrator.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import sys
import time
import uuid

PROTOCOLS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
MAX_BYTES = 65536


def number(description, minimum=None, maximum=None):
    value = {"type": "number", "description": description}
    if minimum is not None:
        value["minimum"] = minimum
    if maximum is not None:
        value["maximum"] = maximum
    return value


def enum(*values):
    return {"type": "string", "enum": list(values)}


TRACK = {"type": "string", "description": "Stable track ID returned by get_context; omitted means selected track."}
SCOPE = {**enum("track", "selection"), "description": "Omitted means the whole track. Always explicitly use selection for 'only here' or a range request, and track for a whole-track request. Refresh get_context immediately before operating if the target may have changed."}
TARGET = {"track_id": TRACK, "scope": SCOPE}
RANGE = {"start_seconds": number("Range start in project seconds", 0),
         "end_seconds": number("Range end in project seconds", 0)}
NAME = {"type": "string", "minLength": 1, "maxLength": 80}


def tool(name, description, properties=None, required=(), readonly=False):
    return {"name": name, "description": description,
            "inputSchema": {"type": "object", "properties": properties or {},
                            "required": list(required), "additionalProperties": False},
            "annotations": {"readOnlyHint": readonly, "destructiveHint": False,
                            "openWorldHint": False}}


TOOLS = [
    tool("get_context", "Read the running DAW's state directly: tracks with stable IDs/settings, selected_track_id, selected_clip_id, selection {has_start,has_end,start_seconds,end_seconds,source}, playback, effects, history and latest_job. Call before interpreting 'here'. Selection source is ab for the marked range or clip for the selected clip. Present relevant results in Codex chat.", readonly=True),
    tool("select_target", "Select the track/clip and optional time range from the user's chat instruction using IDs from get_context. Can establish or change the target directly without asking the user to operate a separate UI.",
         {"track_id": TRACK, "clip_id": {"type": "string"}, **RANGE}),
    tool("adjust_sound", "Set absolute sound controls except gain_db, which is a relative gain change. Read context first. One call is one undoable edit. Selection supports only gain_db, brightness, ambience; repeating the same range updates its existing controls. Partially overlapping ranges are rejected. stability/noise_reduction/de_ess require scope track.",
         {**TARGET, "gain_db": number("Relative gain change in dB", -24, 12),
          "brightness": number("Tone: -1 dark, 0 neutral, 1 bright", -1, 1),
          "ambience": number("Reverb amount", 0, 1), "stability": number("Compression amount", 0, 1),
          "noise_reduction": number("Noise reduction amount", 0, 1), "de_ess": number("Sibilance reduction amount", 0, 1),
          "label": {"type": "string", "maxLength": 120}}),
    tool("add_effect", "Apply an undoable special effect to the selected range, or the explicit project-time range.",
         {"track_id": TRACK, "type": enum("muffled", "radio", "noise", "distortion", "bitcrush"),
          "amount": number("Effect intensity", 0, 1), "wet": number("Wet/dry blend", 0, 1),
          "fade_seconds": number("Fade at both range edges", 0, 10), **RANGE}, ["type"]),
    tool("analyse_audio", "Analyse source audio before effects and explain the result in Codex chat. Scope selection analyses only the selected time range. Returns job_id; poll get_job for completion. Measurements do not determine subjective sound quality.", TARGET, readonly=True),
    tool("get_job", "Read asynchronous analysis, comparison or reference-match progress and result. Wait briefly between polls; only report completion when status confirms it.",
         {"job_id": {"type": "string"}}, ["job_id"], readonly=True),
    tool("compare", "Start or switch comparison directly from chat: loop the last edit before or after with processed-WAV RMS level compensation (not LUFS). May return job_id; poll get_job. Stop exits comparison and restores the edited state. The main DAW also provides before/after/stop buttons for the same operation.",
         {"mode": enum("before", "after", "stop")}, ["mode"]),
    tool("undo_edit", "Undo the latest edit as a group."),
    tool("redo_edit", "Redo the last undone edit."),
    tool("scale_last_edit", "Scale the last assistant adjustment relative to its original state (0 cancels its sound change, 0.5 halves it, 1 restores it).",
         {"factor": number("Change strength", 0, 2)}, ["factor"]),
    tool("create_variants", "Create natural, clear and wide alternatives from the same baseline. Explain the returned settings in chat and let the user choose there. Use returned variant IDs with choose_variant.", TARGET),
    tool("choose_variant", "Apply the alternative selected in chat from create_variants as an undoable edit. The user can switch alternatives through further chat instructions.",
         {"variant_id": {"type": "string"}}, ["variant_id"]),
    tool("match_reference", "Use a reference audio file specified in chat by its local file_path. Analyse up to its first 60 seconds and adjust the target toward it. Returns job_id; poll get_job for completion and explain the result in chat. Does not upload audio or open a file-picker UI.",
         {"track_id": TRACK, "file_path": {"type": "string", "minLength": 1},
          "strength": number("Reference match strength", 0, 1)}, ["file_path"]),
    tool("save_preference", "Save the target's sound controls under a name agreed in chat for later reuse.", {"name": NAME, "track_id": TRACK}, ["name"]),
    tool("apply_preference", "Apply the named preference requested in chat to the target as one undoable edit.", {"name": NAME, **TARGET}, ["name"]),
    tool("list_preferences", "Read saved named sound preferences and present relevant choices in chat.", readonly=True),
    tool("transport", "Control playback. loop_selection requires a selected range; seek requires position_seconds.",
         {"command": enum("play", "pause", "stop", "loop_selection", "seek"),
          "position_seconds": number("Project playback position", 0)}, ["command"]),
]
TOOL_MAP = {item["name"]: item for item in TOOLS}


class BridgeError(RuntimeError):
    pass


def default_control_dir():
    override = os.environ.get("SIMPLE_REC_CONTROL_DIR")
    if override:
        return Path(override).expanduser().resolve()
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")).expanduser()
    return base / "SimpleRecPro" / "CodexControl"


def process_alive(pid):
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return False
    if sys.platform == "win32":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def read_json(path, limit=MAX_BYTES):
    for attempt in range(5):
        try:
            with path.open("rb") as stream:
                data = stream.read(limit + 1)
            break
        except (PermissionError, FileNotFoundError):
            if attempt == 4:
                raise
            time.sleep(0.01)
    if len(data) > limit:
        raise BridgeError("Control message exceeded size limit.")
    value = json.loads(data.decode("utf-8-sig"), parse_constant=lambda v: (_ for _ in ()).throw(ValueError(v)))
    if not isinstance(value, dict):
        raise BridgeError("Control message must be an object.")
    return value


class FileBridge:
    def __init__(self, root=None, timeout=20.0):
        self.root = Path(root or default_control_dir()).resolve()
        self.timeout = max(0.1, min(float(timeout), 55.0))

    def manifest(self):
        # Windows may briefly deny opening a file during atomic replacement.
        # Retry only the read, never an edit request or a failed operation.
        for attempt in range(5):
            try:
                manifest = read_json(self.root / "manifest.json")
                if manifest.get("protocol") != 1:
                    raise BridgeError("Unsupported SimpleRecPro control protocol.")
                if not all(isinstance(manifest.get(k), str) and manifest[k] for k in ("session", "token")):
                    raise BridgeError("Invalid control session.")
                if not process_alive(manifest.get("pid")):
                    raise BridgeError("SimpleRecPro has stopped. Start the application, then read get_context from chat.")
                return manifest
            except (OSError, ValueError) as error:
                if attempt == 4:
                    raise BridgeError("SimpleRecPro is not connected. Start the application and check the MCP connection, then read get_context from chat.") from error
                time.sleep(0.02)

    def directory(self, manifest, key):
        relative = manifest.get(key)
        if not isinstance(relative, str) or Path(relative).is_absolute():
            raise BridgeError("Invalid control directory.")
        path = (self.root / relative).resolve()
        if not path.is_relative_to(self.root) or path == self.root or not path.is_dir():
            raise BridgeError("Control directory is unavailable or outside its root.")
        return path

    def call(self, action, args):
        manifest = self.manifest()
        request_dir = self.directory(manifest, "request_dir")
        response_dir = self.directory(manifest, "response_dir")
        request_id = uuid.uuid4().hex
        request = {"id": request_id, "session": manifest["session"], "token": manifest["token"],
                   "action": action, "args": args, "expires_at_ms": int((time.time() + self.timeout) * 1000)}
        data = json.dumps(request, ensure_ascii=False, allow_nan=False).encode("utf-8")
        if len(data) > MAX_BYTES:
            raise BridgeError("Request too large; no edit applied.")
        pending = request_dir / (request_id + ".tmp")
        target = request_dir / (request_id + ".json")
        response_file = response_dir / (request_id + ".json")
        try:
            with pending.open("xb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(pending, target)
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                current = self.manifest()
                if current["session"] != manifest["session"] or current["token"] != manifest["token"]:
                    raise BridgeError("SimpleRecPro session changed. Read context before trying again; edit completion is unknown.")
                if response_file.exists():
                    response = read_json(response_file, 4 * 1024 * 1024)
                    if response.get("id") != request_id or response.get("session") != manifest["session"]:
                        raise BridgeError("Response session mismatch. Read context before trying again.")
                    if response.get("ok") is not True:
                        raise BridgeError(str(response.get("error", "SimpleRecPro operation failed.")))
                    return response.get("result", {})
                time.sleep(0.025)
            raise BridgeError("SimpleRecPro timed out. The edit may have completed; read context before retrying. Requests are never automatically retried.")
        except OSError as error:
            raise BridgeError(f"Control files became unavailable (OS error {getattr(error, 'winerror', error.errno)}). Read context before retrying; edit completion is unknown.") from error
        finally:
            for own_file in (pending, target, response_file):
                try:
                    own_file.unlink(missing_ok=True)
                except OSError:
                    pass


def validate_args(name, arguments):
    if not isinstance(arguments, dict):
        raise ValueError("arguments must be an object")
    schema = TOOL_MAP[name]["inputSchema"]
    if set(arguments) - set(schema["properties"]):
        raise ValueError("Unknown argument")
    for required in schema["required"]:
        if required not in arguments:
            raise ValueError("Missing argument: " + required)
    for key, value in arguments.items():
        spec = schema["properties"][key]
        if spec["type"] == "number":
            if isinstance(value, bool) or not isinstance(value, (int, float)) or (isinstance(value, float) and not math.isfinite(value)):
                raise ValueError(key + " must be a finite number")
            if value < spec.get("minimum", -math.inf) or value > spec.get("maximum", math.inf):
                raise ValueError(key + " is out of range")
        elif not isinstance(value, str):
            raise ValueError(key + " must be a string")
        elif len(value) < spec.get("minLength", 0) or len(value) > spec.get("maxLength", 4096):
            raise ValueError(key + " has invalid length")
        if "enum" in spec and value not in spec["enum"]:
            raise ValueError(key + " has invalid value")
    if ("start_seconds" in arguments) != ("end_seconds" in arguments):
        raise ValueError("Specify both start_seconds and end_seconds")
    if "end_seconds" in arguments and arguments["end_seconds"] <= arguments["start_seconds"]:
        raise ValueError("end_seconds must be greater than start_seconds")
    if name == "transport" and arguments["command"] == "seek" and "position_seconds" not in arguments:
        raise ValueError("seek requires position_seconds")
    if name == "adjust_sound" and arguments.get("scope") == "selection" and any(
            key in arguments for key in ("stability", "noise_reduction", "de_ess")):
        raise ValueError("stability, noise_reduction and de_ess require scope track")


class Server:
    def __init__(self, bridge):
        self.bridge = bridge
        self.initialized = False

    def handle(self, request):
        request_id = request.get("id") if isinstance(request, dict) else None

        def error(code, message):
            return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}

        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            return error(-32600, "Invalid Request")
        method = request["method"]
        if "id" not in request:
            return None
        if isinstance(request_id, bool) or not isinstance(request_id, (str, int)):
            return error(-32600, "Invalid request id")
        params = request.get("params", {})
        if not isinstance(params, dict):
            return error(-32602, "params must be an object")
        if method == "initialize":
            version = params.get("protocolVersion")
            if not isinstance(version, str) or not isinstance(params.get("capabilities"), dict) or not isinstance(params.get("clientInfo"), dict):
                return error(-32602, "Invalid initialize parameters")
            self.initialized = True
            result = {"protocolVersion": version if version in PROTOCOLS else PROTOCOLS[0],
                      "capabilities": {"tools": {"listChanged": False}},
                      "serverInfo": {"name": "simplerecpro", "version": "1.0.0"},
                      "instructions": "Conduct editing requests, analysis explanations, reference matching, alternative selection and preference management in Codex chat. Read get_context before editing; its selected_track_id, selected_clip_id and selection describe what the user means by 'here'. If the target may have changed, refresh get_context immediately before the operation. For adjust_sound, analyse_audio, create_variants and apply_preference, omitted scope means the whole track: always explicitly send scope selection for a range or 'only here' request and scope track for a whole-track request. Use stable IDs and select_target to change the target directly when the request specifies one. Do not require a separate assistant window. Audio stays local. Use compare to let the user listen; the main DAW also has comparison buttons. Poll get_job to confirm asynchronous completion. Never retry a timed-out edit without checking context."}
        elif method == "ping":
            result = {}
        elif not self.initialized:
            return error(-32002, "Initialize first")
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            name = params.get("name")
            if not isinstance(name, str) or name not in TOOL_MAP:
                return error(-32602, "Unknown tool")
            try:
                arguments = params.get("arguments", {})
                validate_args(name, arguments)
                value = self.bridge.call(name, arguments)
                result = {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False, allow_nan=False)}], "isError": False}
                if isinstance(value, dict):
                    result["structuredContent"] = value
            except (BridgeError, ValueError, TypeError) as exception:
                result = {"content": [{"type": "text", "text": str(exception)}], "isError": True}
        else:
            return error(-32601, "Method not found")
        return {"jsonrpc": "2.0", "id": request_id, "result": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-dir", type=Path)
    parser.add_argument("--timeout", type=float, default=20.0)
    options = parser.parse_args()
    server = Server(FileBridge(options.control_dir, options.timeout))
    source = sys.stdin.buffer
    while True:
        line = source.readline(MAX_BYTES + 1)
        if not line:
            return
        if len(line) > MAX_BYTES:
            # Do not accidentally parse the tail of an oversized request as a new message.
            while line and not line.endswith(b"\n"):
                line = source.readline(MAX_BYTES + 1)
            response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Message too large"}}
        else:
            try:
                request = json.loads(line.decode("utf-8"), parse_constant=lambda v: (_ for _ in ()).throw(ValueError(v)))
                response = server.handle(request)
            except (ValueError, UnicodeError):
                response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
        if response is not None:
            sys.stdout.buffer.write((json.dumps(response, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8"))
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
