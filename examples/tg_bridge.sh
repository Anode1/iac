#!/usr/bin/env bash
# tg_bridge.sh -- relay one Telegram chat <-> one iac name (a reference bridge).
#
# It makes YOU a name on the board, reachable from your phone: your Telegram
# messages are posted to the board as that name, and any board message addressed
# to that name is delivered to your phone. The board (iac) does the fan-out and
# coordination -- this bridge only carries bytes in and out, one chat, two ways.
#
# Typical use: your phone -> a "manager" agent that distributes work to others.
#
#   your phone (Telegram)  <->  tg_bridge.sh  <->  iac board  <->  manager agent
#                                                                   fans out via
#                                                                   iac send/?/*
#
# Deps: bash, curl, jq, base64 (coreutils). Needs no public endpoint -- Telegram
# long-polling reaches out, so it runs fine on the board's own host behind NAT.
#
#   TG_TOKEN=<botfather-token> TG_CHAT=<your-chat-id> IAC_ROOM=/tmp/room \
#     IAC=./iac ./examples/tg_bridge.sh
#
# Get TG_TOKEN from @BotFather; get TG_CHAT by messaging your bot once, then
# reading .message.chat.id from  curl "https://api.telegram.org/bot$TG_TOKEN/getUpdates".
set -euo pipefail

IAC=${IAC:-iac}                     # the iac binary
HUMAN=${HUMAN:-phone}               # the name this bridge holds on the board (you)
TARGET=${TARGET:-hub}               # where your messages go (the manager agent)
: "${TG_TOKEN:?set TG_TOKEN (from @BotFather)}"
: "${TG_CHAT:?set TG_CHAT (your chat id -- the ONLY chat this bridge trusts)}"
: "${IAC_ROOM:?set IAC_ROOM (the board directory)}"
API="https://api.telegram.org/bot$TG_TOKEN"

tg_send() {                         # $1 = text -> your Telegram chat
    curl -sS -X POST "$API/sendMessage" \
        --data-urlencode "chat_id=$TG_CHAT" \
        --data-urlencode "text=$1" >/dev/null || true
}

trap 'kill 0' EXIT INT TERM         # tear down the egress child with us

# EGRESS: board -> Telegram. Block on the board (a real wakeup); forward each
# message for HUMAN to the phone. Loops forever, re-arming on every idle timeout.
( while :; do
      msg=$("$IAC" recv "$IAC_ROOM" "$HUMAN" 300 2>/dev/null) && tg_send "$msg"
  done ) &

# INGRESS: Telegram -> board. Long-poll for messages; post each (from the trusted
# chat only) to TARGET, as HUMAN. base64 keeps multi-line/odd text intact.
echo "[tg-bridge] up: '$HUMAN' <-> Telegram chat $TG_CHAT; your messages -> '$TARGET'" >&2
offset=0
while :; do
    resp=$(curl -sS "$API/getUpdates?offset=$offset&timeout=25&allowed_updates=%5B%22message%22%5D") \
        || { sleep 2; continue; }
    while IFS=$'\t' read -r id chat text64; do
        offset=$((id + 1))
        [ "$chat" = "$TG_CHAT" ] || continue          # ignore anyone but you
        text=$(printf '%s' "$text64" | base64 -d 2>/dev/null) || continue
        [ -n "$text" ] || continue
        IAC_FROM="$HUMAN" "$IAC" send "$IAC_ROOM" "$TARGET" -- "$text"
    done < <(printf '%s' "$resp" | jq -r '.result[]
                | "\(.update_id)\t\(.message.chat.id // "")\t\(.message.text // "" | @base64)"')
done
