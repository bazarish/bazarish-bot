# The REST API

Everything a bot does, it does here. The daemon holds the account; the automation
holds a token.

## Opening it

Every call carries the token in the Authorization header:

```
Authorization: Bearer <token>
```

Tokens are issued in the web interface, under a name, and shown once. A call
with no token and a call with a token that is not one of this daemon's are told
apart, because those are different mistakes:

```json
{"error": "the API is opened by a token in the Authorization header"}
{"error": "this token does not open the API"}
```

While the account is closed or still coming up, every route answers `503` with
the status in it: `{"error": "the account is not open: starting"}`.

## Routes

| Route | What it does |
|---|---|
| `GET /v1/status` | the account's fingerprint, name, address and the id of the last event |
| `GET /v1/invite` | the invite URI to hand out to be added as a contact |
| `GET /v1/contacts` | the contacts, each with `pending` for a request not yet accepted and `tokens` for how many more messages can be sent to them |
| `POST /v1/contacts` | `{"invite": "...", "text": "..."}` adds one; `{"accept": "<fingerprint>"}` accepts a request |
| `GET /v1/chats` | the last line of every conversation |
| `GET /v1/messages?peer=&before=&limit=` | a conversation, newest first |
| `POST /v1/messages` | `{"peer": "...", "text": "...", "keyboard": [...]}` |
| `POST /v1/messages/{id}` | `{"peer": "...", "text": "..."}` revises a message this account sent |
| `DELETE /v1/messages/{id}?peer=` | takes a message this account sent back, on both sides |
| `POST /v1/reactions` | `{"peer": "...", "messageId": "...", "emoji": "👍"}`; an empty emoji takes it back |
| `POST /v1/receipts` | `{"peer": "...", "messageId": "..."}` tells the peer their message arrived |
| `POST /v1/tokens` | `{"peer": "...", "batches": 2}` asks that contact for more of their tokens |
| `POST /v1/chats/{fingerprint}/clear` | asks the peer to drop the conversation, and drops this side of it |
| `GET /v1/events?since=&wait=` | everything that happened after `since`, waiting up to `wait` seconds |

### Sending

```bash
curl -s -X POST http://127.0.0.1:8440/v1/messages \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "text": "hello"}'
```

```json
{"messageId": "5373671a07d8b517", "confirmed": false}
```

`confirmed` is whether the far side's server had stored it before the wait was
up. **False is not a failure.** The mailbox took the message and keeps
delivering it in the background with retries, exactly as it does for the desktop
client; a bot that treats false as an error will resend what has already been
sent.

An inline keyboard is rows of buttons, each with a `text` and either a `data`
(the peer's tap sends a `bot.callback` back) or a `command`:

```json
{"peer": "...", "text": "Pick one:",
 "keyboard": [[{"text": "Ping", "data": "ping"}, {"text": "Help", "command": "help"}]]}
```

### Waiting for something to happen

```bash
curl -s "http://127.0.0.1:8440/v1/events?since=$LAST&wait=60" \
  -H "Authorization: Bearer $TOKEN"
```

```json
{"events": [ ... ], "lastEventId": 42}
```

The daemon holds the request open until there is something after `since` or the
wait is up, so a loop over this neither spins nor lags. Pass the `lastEventId`
you were given as the next `since`; start at `0` to be handed everything the
daemon remembers.

An event says what happened, and to which message:

```json
{"id": 42, "kind": "message", "peer": "<fingerprint>",
 "messageId": "4054cd273aa34b4b", "text": "hello", "sentAt": 1787932486147}
```

`kind` is one of `message`, `edit`, `delete`, `reaction`, `receipt` or `status`. Only a
`message` is a new line; the other four are about a line that is already there,
so a bot that keeps its own copy applies them to it. Read the conversation back
with `GET /v1/messages` and each line carries its reactions and whether it has
been edited:

```json
{"id": 5, "messageId": "b69ccb66654dd3fb", "outgoing": false, "type": "text",
 "text": "123", "edited": false, "reactions": [{"reactor": "me", "emoji": "👍"}]}
```

A reaction's `reactor` is `me` for this account's own, and the peer's
fingerprint for theirs.

A line's `type` is the end-to-end content type - `text`, `contact.request`,
`bot.command`, `bot.callback`. A bot answering commands watches for
`bot.command` and for text beginning with a slash, which the protocol says are
two spellings of one thing.

## A bot in ten lines

```bash
TOKEN=...; SINCE=0
while :; do
  ANSWER=$(curl -s "http://127.0.0.1:8440/v1/events?since=$SINCE&wait=60" \
           -H "Authorization: Bearer $TOKEN")
  SINCE=$(echo "$ANSWER" | jq -r .lastEventId)
  echo "$ANSWER" | jq -c '.events[] | select(.outgoing == false and .type == "text")' |
  while read -r LINE; do
    PEER=$(echo "$LINE" | jq -r .peer)
    curl -s -X POST http://127.0.0.1:8440/v1/messages \
      -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
      -d "{\"peer\": \"$PEER\", \"text\": \"you said: $(echo "$LINE" | jq -r .text)\"}" > /dev/null
  done
done
```

## Sending in bulk

A bot that sends notices rather than answers has two failure modes worth
watching, and both are visible here.

**Capacity.** Every message spends a one-time token the recipient issued. `GET
/v1/contacts` reports what is left:

```json
[{"fingerprint": "...", "name": "alice", "pending": false, "tokens": 41}]
```

The stash refills itself: below a threshold the next message asks the recipient
for a fresh batch and prepays the reply, so a conversation does not run dry
while both sides are about. What it cannot do is refill against a recipient who
is not there - their client is what mints the tokens. **At zero, sends fail and
keep failing until they come back.** A bot that queues thousands of notices
should read `tokens` and stop, rather than turn every one of them into a
failure.

**Building a stash.** A conversation refills on its own traffic, which suits two
people talking and not a service that sends far more than it receives. Ask for
more while the other side is about:

```bash
curl -s -X POST http://127.0.0.1:8440/v1/tokens \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "batches": 4}'
```

Each ask is a delivery of its own - nothing is shown to the person on the other
end - and each brings back a batch of 64. It spends one token to gain sixty-four,
and up to 8 asks are allowed per call. The batches arrive when the other side's
client next syncs, so read `tokens` afterwards rather than assuming: an ask to
somebody who is not running answers nothing until they are. With no tokens at
all there is nothing to ask with, and the call says so.

**Delivery.** `POST /v1/messages` answers as soon as the mailbox takes the
message, and `confirmed` says only whether the far side signed for it inside the
send's own wait. What became of it afterwards is on the line and in the feed:

```json
{"id": 12, "messageId": "5373671a07d8b517", "outgoing": true, "text": "notice",
 "status": "delivered"}
```

`status` is `sending` while the courier is working, `delivered` once the
recipient's server has signed for it, or `failed: <reason>`. Every change is an
event of kind `status` carrying the same text, so a long poll sees it without
re-reading the conversation.

## Taking things back

`DELETE /v1/messages/{id}?peer=<fingerprint>` removes a message this account
sent, on both sides, leaving no tombstone. When the *peer* takes one of theirs
back, it arrives as an event of kind `delete` naming the `messageId`, and the
line is gone from the conversation.

`POST /v1/chats/{fingerprint}/clear` asks the peer to drop the whole
conversation and drops this side of it at once; the far side honours it the way
every client does. When the peer asks the same of this account, the conversation
here is dropped and an event of kind `chat-cleared` says so.

## Reactions the other side puts on a message

They arrive as events of kind `reaction`, with `messageId` naming the message,
`actor` naming who reacted (`me` for this account) and `text` carrying the emoji
- an empty `text` means the reaction was taken back:

```json
{"id": 22, "kind": "reaction", "peer": "...", "messageId": "d148fadc4a9c",
 "actor": "...", "text": "😂"}
```

Reading the conversation back gives the same thing settled, on the line it
belongs to: each message carries a `reactions` array of `{reactor, emoji}`.
