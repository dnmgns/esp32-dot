#!/usr/bin/env python3
"""Answer spoken transcripts with Claude and show the answer on the device.

Watches the bridge for `transcript` events, asks the local `claude` CLI (print mode,
no tools), and posts the reply as a notice card. Needs a running bridge
(scripts/bridge.py) and a logged-in `claude` on PATH.
"""
import json
import os
from pathlib import Path
import sys
import time
import subprocess
import tempfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = os.environ.get("PIP_BRIDGE", "http://127.0.0.1:8788")
MODEL = os.environ.get("PIP_MODEL", "haiku")
SYSTEM = (
    "You answer questions spoken to a small round screen on a desk device. "
    "Reply in plain text only, at most 3 short sentences and under 400 characters, "
    "with no markdown. You cannot browse the web; if you do not know something "
    "current, say so briefly."
)
MAX_BODY = 600


def call(url, body=None, headers=None):
    request = urllib.request.Request(
        url, data=None if body is None else json.dumps(body).encode(),
        headers=dict({"Content-Type": "application/json"}, **(headers or {})))
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def ask(history):
    transcript = "\n".join(
        ("User: " if turn["role"] == "user" else "You: ") + turn["content"] for turn in history)
    result = subprocess.run(
        ["claude", "-p", "--model", MODEL, "--system-prompt", SYSTEM, "--tools", "",
         "--no-session-persistence", "--disable-slash-commands", "--setting-sources", ""],
        input=transcript + "\nYou:", capture_output=True, text=True, timeout=90,
        cwd=tempfile.gettempdir())
    if result.returncode != 0 or not result.stdout.strip():
        raise ValueError(result.stderr.strip()[:150] or "empty answer")
    return result.stdout.strip()


def show(text):
    call(f"{BRIDGE}/cards", {"kind": "notice", "body": text[:MAX_BODY] or "(no answer)"})


def main():
    events = call(f"{BRIDGE}/events?after=0")
    seen = events[-1]["seq"] if events else 0   # skip transcripts from before we started
    history = []
    print(f"Responder running with {MODEL}; speak to the device.", flush=True)
    while True:
        try:
            for event in call(f"{BRIDGE}/events?after={seen}"):
                seen = max(seen, event["seq"])
                if event.get("type") != "transcript" or not event.get("text", "").strip():
                    continue
                print(f"heard: {event['text']}", flush=True)
                history.append({"role": "user", "content": event["text"]})
                try:
                    answer = ask(history[-12:])
                except (subprocess.SubprocessError, ValueError) as error:
                    history.pop()
                    answer = f"Claude failed: {error}"[:200]
                else:
                    history.append({"role": "assistant", "content": answer})
                print(f"reply: {answer}", flush=True)
                show(answer)
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass   # bridge restarting
        time.sleep(1)


if __name__ == "__main__":
    main()
