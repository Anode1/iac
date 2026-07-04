#!/usr/bin/env bash
# wa_bridge.sh -- relay WhatsApp (Business Cloud API) <-> one iac name.
#
# STATUS: UNFINISHED -- a working starting point, NOT production-ready. The webhook
# mechanics (verify handshake, trusted-number filter, message -> board) are tested
# only against canned payloads locally, never a live WhatsApp Business account.
# Before real use, add at least:
#   - webhook signature verification (check Meta's X-Hub-Signature-256 with your
#     app secret) -- without it, anyone who reaches the URL can POST fake messages;
#   - the 24-hour-window / message-template flow for egress: a plain-text reply
#     sent >24h after your last inbound message is REJECTED by Meta and needs an
#     approved template (this bridge only sends plain text);
#   - handling for non-text messages, media, and status callbacks;
#   - a public HTTPS endpoint in front of PORT (tunnel or relay VPS).
# The Telegram bridge (tg_bridge.sh) has none of these gaps -- prefer it if you can.
#
# Same idea as tg_bridge.sh -- you become a name on the board, reachable from
# WhatsApp -- but WhatsApp is WEBHOOK-based, not long-poll: Meta POSTs inbound
# messages to a URL you serve. So, unlike the Telegram bridge:
#   - egress (board -> WhatsApp) is one curl to the Cloud API (the "swap");
#   - ingress (WhatsApp -> board) runs a small HTTP webhook server, and needs a
#     PUBLIC HTTPS URL -- iac is same-host, so tunnel to this port (cloudflared /
#     ngrok) or run the bridge on a relay VPS. Point the Meta app's webhook at it.
#
#   your phone (WhatsApp) <-> Meta Cloud API <-> wa_bridge.sh <-> iac board <-> agents
#
# Deps: bash, curl, python3 (for the webhook server + JSON). Env:
#   WA_TOKEN     Cloud API access token (Meta, or a BSP like Twilio)
#   WA_PHONE_ID  your WhatsApp phone-number id (the sender)
#   WA_TO        your number in E.164 digits, e.g. 15551234567 -- the ONLY peer trusted
#   WA_VERIFY    the webhook verify token you set in the Meta app config
#   PORT         local webhook port (default 8080; put HTTPS in front of it)
#   IAC_ROOM, IAC, HUMAN, TARGET  -- as in tg_bridge.sh (defaults: iac, phone, hub)
set -euo pipefail

IAC=${IAC:-iac}; HUMAN=${HUMAN:-phone}; TARGET=${TARGET:-hub}; PORT=${PORT:-8080}
: "${WA_TOKEN:?set WA_TOKEN}"; : "${WA_PHONE_ID:?set WA_PHONE_ID}"
: "${WA_TO:?set WA_TO (your E.164 number, the only trusted peer)}"
: "${WA_VERIFY:?set WA_VERIFY (webhook verify token)}"; : "${IAC_ROOM:?set IAC_ROOM}"
GRAPH="https://graph.facebook.com/v20.0"

wa_send() {                         # $1 = text -> your WhatsApp
    local payload
    payload=$(WA_TO="$WA_TO" python3 -c 'import os,sys,json; print(json.dumps(
        {"messaging_product":"whatsapp","to":os.environ["WA_TO"],
         "type":"text","text":{"body":sys.argv[1]}}))' "$1")
    curl -sS -X POST "$GRAPH/$WA_PHONE_ID/messages" \
        -H "Authorization: Bearer $WA_TOKEN" -H "Content-Type: application/json" \
        -d "$payload" >/dev/null || true
}

trap 'kill 0' EXIT INT TERM         # tear down the egress child with us

# EGRESS: board -> WhatsApp. Block on the board (a real wakeup); forward each
# message for HUMAN. Identical to the Telegram bridge except the send call.
( while :; do
      msg=$("$IAC" recv "$IAC_ROOM" "$HUMAN" 300 2>/dev/null) && wa_send "$msg"
  done ) &

# INGRESS: the Cloud API webhook. Meta GETs once to verify (echo hub.challenge if
# the token matches), then POSTs each inbound message; we post messages from the
# trusted number to TARGET as HUMAN. Ack POSTs fast (Meta retries on non-200).
echo "[wa-bridge] up: '$HUMAN' <-> WhatsApp $WA_TO; webhook on :$PORT -> '$TARGET'" >&2
IAC="$IAC" IAC_ROOM="$IAC_ROOM" HUMAN="$HUMAN" TARGET="$TARGET" \
WA_TO="$WA_TO" WA_VERIFY="$WA_VERIFY" PORT="$PORT" python3 - <<'PY'
import os, json, subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

IAC, ROOM = os.environ["IAC"], os.environ["IAC_ROOM"]
HUMAN, TARGET = os.environ["HUMAN"], os.environ["TARGET"]
WA_TO, VERIFY = os.environ["WA_TO"], os.environ["WA_VERIFY"]

def to_board(text):
    subprocess.run([IAC, "send", ROOM, TARGET, "--", text],
                   env={**os.environ, "IAC_FROM": HUMAN})

class Webhook(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):                       # Meta verification handshake
        q = parse_qs(urlparse(self.path).query)
        if q.get("hub.verify_token", [None])[0] == VERIFY and "hub.challenge" in q:
            self.send_response(200); self.end_headers()
            self.wfile.write(q["hub.challenge"][0].encode())
        else:
            self.send_response(403); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        raw = self.rfile.read(n)
        self.send_response(200); self.end_headers()     # ack first, then process
        try:
            d = json.loads(raw)
        except Exception:
            return
        for entry in d.get("entry", []):
            for ch in entry.get("changes", []):
                for m in ch.get("value", {}).get("messages", []):
                    if m.get("from") != WA_TO:           # trust only your number
                        continue
                    text = (m.get("text") or {}).get("body")
                    if text:
                        to_board(text)

HTTPServer(("0.0.0.0", int(os.environ["PORT"])), Webhook).serve_forever()
PY
