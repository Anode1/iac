# Integrating external messengers (Telegram, WhatsApp, …)

`iac` is same-host by design: a board is files on one machine. To reach it from
your phone -- to text a fleet of agents and get their replies back -- you put a
thin **bridge** between an external messenger and the board. The bridge does not
coordinate anything; it only carries bytes in and out. The board already does the
hard part.

    your phone (Telegram / WhatsApp / …)
          │   the messenger's API (long-poll or webhook)
          ▼
       bridge  ── iac send <room> <manager> "<your text>"   (you, as a name)
          ▲               │
          │               ▼
          │        manager agent (parked on iac recv) wakes, fans out to
          │        workers via iac send / * / ?, collects results, coordinates
          │               │
          └── messenger send ◄── a board message addressed to your name

The key idea is iac's own: **a human is just another name on the board.** The
bridge *is* that name. Your messages arrive as sends from it; any board message
addressed to it is pushed to your phone. Point your messages at a "manager" agent
and it distributes and coordinates the work -- that is agent logic, not bridge
logic.

## Why a bridge, not MCP

They solve different layers, and neither replaces iac:

- **The messenger API** is *remote ingress* -- how a message gets from your phone
  to your machine.
- **`iac`** is *local fan-out and coordination* -- it already does the "distribute
  to other agents, manager coordinates" part.
- **MCP** exposes *tools to an agent*; it does not receive messenger messages. You
  could wrap "send to Telegram" as an MCP tool, but it is optional and not the
  ingress path.

So the whole integration is the ~40-line bridge below. iac is unchanged.

## Telegram, the reference bridge

Telegram is by far the simplest for this: a free bot token from `@BotFather`, no
business verification, **no 24-hour window and no template approval** (your agent
can message you anytime once you have started the chat), and **long-polling that
needs no public endpoint** -- it reaches out, so the bridge runs fine on the
board's own host behind NAT.

[`examples/tg_bridge.sh`](../examples/tg_bridge.sh) is a complete two-way bridge
(bash + `curl` + `jq`). It makes you the name `phone` on the board and sends your
messages to a manager agent `hub`:

    # 1. get a bot token from @BotFather; 2. message your bot once, then read your
    #    chat id from getUpdates; 3. run the bridge on the board's host:
    TG_TOKEN=<token> TG_CHAT=<your-chat-id> IAC_ROOM=/tmp/room IAC=./iac \
      ./examples/tg_bridge.sh

    # your manager agent, parked on the board, coordinates the fleet:
    iac recv /tmp/room hub 500            # wakes on your message
    #   ... it then: iac send /tmp/room '?' "<subtask>"   (dispatch to a worker)
    #                iac send /tmp/room '*' "<all-hands>"  (broadcast)
    #                iac send /tmp/room phone "done: <result>"   (reply to you)

`HUMAN` (your board name), `TARGET` (the agent your messages go to), and `IAC`
(the binary) are overridable by env; see the script header.

## WhatsApp -- official API, more setup, real constraints

> **Status: incomplete.** [`examples/wa_bridge.sh`](../examples/wa_bridge.sh) is a
> working *starting point*, not production-ready: its webhook mechanics are tested
> only against canned payloads locally, never a live WhatsApp Business account. It
> still needs webhook **signature verification** (Meta's `X-Hub-Signature-256`),
> the **24-hour-window / template flow** for proactive replies, and handling for
> non-text messages -- see the caveats in the script header. Prefer Telegram unless
> you specifically need WhatsApp.

WhatsApp *does* have an official API -- the **WhatsApp Business Cloud API** (Meta)
-- but it is heavier than Telegram for this use case:

- **Setup is moderate:** a Meta Business account, a WhatsApp Business Account, a
  dedicated phone number, an app in Meta for Developers, and a **public HTTPS
  webhook** for inbound messages. **Twilio** (or 360dialog) wraps the Cloud API
  and has a *sandbox* you can test in minutes before full onboarding.
- **The real gotcha -- the 24-hour window:** you may reply freely only within 24h
  of the user's last message. To message the user *proactively* after that (e.g.
  "the job you started hours ago finished") you must use a **pre-approved message
  template**. For an agent that notifies you later, this is the main friction, and
  the reason Telegram is easier here.
- **Public endpoint:** the webhook needs a reachable HTTPS URL. Since iac is
  same-host, either tunnel to the board's host (`cloudflared` / `ngrok`) or run
  the bridge on a small VPS that relays into the board.

The iac side is untouched; only the messenger edges change -- but not symmetrically.
**Egress** is a straight swap: a Cloud API `POST /messages` instead of Telegram's
`sendMessage`. **Ingress is different in kind:** Telegram long-polls (the bridge
reaches out), but WhatsApp *pushes* -- Meta POSTs to a webhook -- so the bridge must
run a small HTTP server and expose it over HTTPS, rather than poll.
[`examples/wa_bridge.sh`](../examples/wa_bridge.sh) shows both: `curl` for egress,
a tiny `python3` webhook (verify handshake + trusted-number filter + `iac send`)
for ingress.

> Avoid the unofficial libraries (Baileys, whatsapp-web.js): they drive WhatsApp
> Web against Meta's terms and risk a number ban.

## Other channels

Same pattern, different two calls:

- **Slack / Discord** -- a bot token; Events API / gateway inbound, `chat.postMessage`
  / webhook outbound. Easy, no message-window limits.
- **SMS (Twilio)** -- simplest of all, but per-message cost and no rich text.
- **Signal** -- `signal-cli` in JSON-RPC mode; workable, clunkier to run.
- **Email** -- an IMAP poll in, an SMTP send out; high-latency but zero platform.

## Security -- the bridge is a trust boundary

Within a room, iac is cooperative: anyone who can post is a full participant, and
`IAC_FROM` is an unverified label (see the README **Trust model**). So the bridge
is where you *authenticate the outside world*:

- **Allow exactly one identity.** `tg_bridge.sh` honors only `TG_CHAT`; a stranger
  who finds your bot is ignored. Do the equivalent for any channel (verify the
  sender / a webhook secret) -- otherwise anyone who reaches the bridge can drive
  your fleet.
- **Keep tokens out of the board.** The bot token lives in the bridge's
  environment, never in a message.
- **One board per trust domain.** The bridge injects into a specific room; do not
  point it at a room shared with agents you would not let a remote human command.

## Deployment note

Run the bridge on the machine that holds the board (that is where `iac` and the
room files are). Telegram/Slack/Discord long-poll or gateway-connect outbound, so
they need no inbound port. WhatsApp/SMS webhooks need a public HTTPS URL -- tunnel
to the host, or put the bridge on a relay VPS. See also
[`doc/ORCHESTRATION.md`](ORCHESTRATION.md) (the human as a name on the board) and
[`doc/dev/RECEIVE_MODEL.md`](dev/RECEIVE_MODEL.md) (§6, the keyboard-priority
driver -- a messenger is just another input channel feeding the board).
