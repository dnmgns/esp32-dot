---
name: magnus-dot
description: Send messages, questions and quick decisions to Magnus's desk display (a Seeed SenseCAP Watcher running esp32-dot), and handle his spoken input and dial replies. Use when Magnus asks to show something on the device, wants to be asked a question there, or is talking to it.
---

This is Magnus's own variant of `rsms-dot`. The device is a **Seeed SenseCAP Watcher
W1-A**: a round 412 x 412 touchscreen with a rotary dial and push button, a
microphone, and Wi-Fi. It is not the original rectangular Waveshare display, so
ignore layout numbers from `rsms-dot`.

## When to use the display

Only put things on the device that Magnus asked for. Ordinary chat does not belong
there, and no reply should go to the display just because it can. Typical requests:

- an answer or result he wants on the desk ("put that on the Watcher")
- a yes/no or pick-one approval he asked to be asked about
- a short reminder or glanceable fact
- a reply to something he said into the microphone (see Voice)

Do not echo dismissals, and do not send status chatter unprompted.

## How to reach it

The bridge runs on Magnus's Mac (`scripts/bridge.py`, usually started by hand) and
the Watcher joins it over Wi-Fi. Check it first:

```sh
curl -s http://127.0.0.1:8788/health        # want "connected": true
```

If `connected` is false, say so and stop. Do not claim a message was shown. The
Watcher may be asleep, off Wi-Fi, or the bridge may be down. A sent card is only
*queued*, not displayed or read.

Send cards with the local HTTP API (no MCP needed from Claude Code):

```sh
# notice: shown on a yellow attention screen; Magnus taps it to read, then dismisses
curl -s http://127.0.0.1:8788/cards -H 'Content-Type: application/json' \
  -d '{"kind":"notice","body":"Build finished."}'

# decision: one to three options
curl -s http://127.0.0.1:8788/cards -H 'Content-Type: application/json' \
  -d '{"kind":"decision","body":"Deploy now?","options":[{"id":"yes","label":"Yes"},{"id":"no","label":"No"}]}'
```

Optional fields: `id` (use your own for retries), `title`. Notices take no custom
options. Read results with:

```sh
curl -s http://127.0.0.1:8788/cards              # status: pending | answered, option_id
curl -s "http://127.0.0.1:8788/events?after=0"   # choices and transcripts, ordered by seq
```

An MCP adapter also exists (`scripts/mcp-stdio.py`) if Magnus registers it.

## Writing for the screen

- Plain ASCII only (printable characters and newlines). No emoji, no curly quotes,
  no accents, no Markdown.
- One short sentence if you can. Hard limit 600 characters. The firmware pages the
  text at about four lines per page, so keep it far shorter than that.
- Option labels at most 64 characters, short enough to scan. Use stable option IDs
  that do not depend on the label.
- Do not use `urgent` or interrupt a message unless the delay matters.

## How Magnus answers

- **Tap** the screen or **short-press the dial** to open a yellow message and to
  page forward. Turn the dial **right** for the next page or option, **left** to go back.
- A decision shows each option on its own page. Short-press on the option opens a
  **Confirm** screen, and a second short-press submits. Turning cancels the confirm.
- A reply arrives as a `choice` event with the card's `option_id`. That answers that
  one question. Dismissing a notice only acknowledges it. Act only within what
  Magnus has authorized.

## Voice

Magnus can talk to the device by **holding the screen** or **long-pressing the dial**
from idle or sleep, speaking, and releasing. The bridge transcribes locally with
Whisper and records a `transcript` event with `text`. Transcripts can contain
recognition errors, so clarify uncertain names, amounts, or consequential actions
before acting. Deduplicate by event `id`.

`scripts/responder.py` is a separate local voice loop: it answers each transcript
with the `claude` CLI and posts the answer as a notice card. If it is running, do not
also answer the same transcript from another session, or Magnus gets two cards. Ask
before answering transcripts yourself.

When Magnus has told you to treat the microphone as an ongoing interface, reply both
in the chat and on the device. The chat can carry more detail, and the device text
must stay concise and say the same thing.

## Etiquette

- Generate a unique `id` per new card. Reuse the same `id` and content only to retry
  a call you are unsure about.
- Never put secrets (passwords, keys, tokens) on the display.
- If the device is unreachable, tell Magnus in chat instead of retrying silently.
