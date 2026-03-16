# DIRECTPROXY

`directproxy` adds native browser signaling support to `directcall` by exposing a WebSocket endpoint while preserving the original direct app-to-app TCP behavior.

## Overview

- Existing direct signaling remains available over TCP (`directcall` listener, default `3456`).
- A WebSocket endpoint (default `3457`) is exposed for browser clients.
- The WS side is bridged into the existing direct protocol path, so call logic stays in `directcall`.
- Browser and native clients use the same message vocabulary (`HELLO`, `INVITE`, `WAITING`, `OFFER`, `ANSWER`, `ICE`, `BYE`, `CANCEL`, etc.).

## Ports and Transport

- Direct signaling TCP: `127.0.0.1:3456` (default in examples)
- WebSocket signaling: `ws://<host>:3457`

The WS proxy uses text frames on the browser side and forwards messages to direct TCP with a 2-byte big-endian length prefix expected by direct signaling internals.

## Configuration

New `directcall` options:

- `--websocket_signaling` / `--no-websocket_signaling`
- `--websocket_port=<port>`
- `--talking_face=<path_to_image>`

JSON config equivalents are supported in `directcall.config.example.json`.

## Direct Protocol Messages

Common messages:

- `HELLO:<target_id>`
- `200 OK`
- `400 Bad Request`
- `480 Temporarily Unavailable`
- `486 Busy Here`
- `INVITE:<json_or_payload>`
- `WAITING`
- `OFFER:<sdp>`
- `ANSWER:<sdp>`
- `ICE:<mline>:<candidate>`
- `BYE`
- `CANCEL`

Optional/legacy side messages still recognized by protocol family:

- `REGISTER:<id>:<room>`
- `USERS`
- `ADDRESS:<id>:ip:port`
- `WHISPER[xx]<text>`
- `LLAMA[xx]<text>`

## Browser Call Flow (Expected Sequence)

1. Browser connects to `ws://host:3457`
2. Browser sends `HELLO:<target>`
3. Server replies `200 OK`
4. Browser sends `INVITE:{"agent":"audio","room_name":"room101","video":true}`
5. Server replies `WAITING`
6. Browser creates offer, sends `OFFER:<sdp>`
7. Server replies `ANSWER:<sdp>`
8. Both sides exchange `ICE:<mline>:<candidate>`
9. Session ends with `BYE` or `CANCEL`

## Talking Face Mode

When built with speech/talking-face support and started with:

`--talking_face=/path/to/image`

the callee can provide a synthetic video source generated from the image and speech activity. If unavailable, code can fall back to synthetic echo video source (depending on runtime branch behavior).

## Sample Browser Page

A complete sample is provided at:

- `ws_test.html`

It includes:

- local/remote audio+video elements
- strict direct protocol sequencing (`HELLO -> 200 OK -> INVITE -> WAITING -> OFFER`)
- SDP offer normalization for this branch
- ICE exchange
- remote video state probes

Minimal JS signaling skeleton:

```html
<script>
const ws = new WebSocket("ws://127.0.0.1:3457");
ws.onopen = () => ws.send("HELLO:Slim");
ws.onmessage = async (e) => {
  const msg = String(e.data || "");
  if (msg.startsWith("200 OK")) {
    ws.send('INVITE:{"agent":"audio","room_name":"room101","video":true}');
  } else if (msg.startsWith("WAITING")) {
    // createOffer + setLocalDescription
    // ws.send(`OFFER:${offerSdp}`);
  } else if (msg.startsWith("ANSWER:")) {
    // setRemoteDescription(answer)
  } else if (msg.startsWith("ICE:")) {
    // addIceCandidate(...)
  }
};
</script>
```

## Run Example

Start callee with WS signaling and talking face:

```bash
./out/directproxy_whillats/directcall \
  --mode=callee \
  --user_name=Slim \
  --room_name=room101 \
  --encryption \
  --video \
  --webrtc_cert_path=cert.pem \
  --webrtc_key_path=key.pem \
  --websocket_signaling \
  --websocket_port=3457 \
  --talking_face="/absolute/path/RobotPhoneLogo.jpeg" \
  127.0.0.1:3456
```

Then open:

- `http://127.0.0.1:8000/ws_test.html`

