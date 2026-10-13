# The REST API

Everything a bot does, it does here. The daemon holds the accounts; the
automation holds a token, and a token opens the one account it was issued for.

## Opening it

Every call carries the token in the Authorization header:

```
Authorization: Bearer <token>
```

Tokens are issued in the web interface, per account, under a name, and shown
once. The routes below have no account in them: the token says which. A call
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
| `GET /v1/status` | the account's fingerprint, name, kind (`bot` or `group`), address and the id of the last event |
| `GET /v1/invite` | the invite URI to hand out to be added as a contact |
| `GET /v1/contacts` | the contacts, each with `pending` for a request not yet accepted, `accepting` while the acceptance is on its way, and `writable` for whether this account holds the pass that admits it to their mailbox |
| `POST /v1/contacts` | `{"invite": "...", "text": "..."}` or `{"alias": "!name", "text": "..."}` adds one; `{"accept": "<fingerprint>"}` accepts a request |
| `DELETE /v1/contacts/{fingerprint}` | removes a contact and revokes the pass it was issued, or declines a request; the conversation stays |
| `GET /v1/chats` | the last line of every conversation |
| `GET /v1/messages?peer=&before=&limit=` | a conversation, newest first |
| `POST /v1/messages` | `{"peer": "...", "text": "...", "replyTo": "<messageId>", "keyboard": [...]}`; `replyTo` and `keyboard` are optional |
| `POST /v1/forward` | `{"from": "<fingerprint>", "messageId": "...", "to": "<fingerprint>"}` sends a text message on, marked forwarded |
| `POST /v1/messages/{id}` | `{"peer": "...", "text": "..."}` revises a message this account sent |
| `DELETE /v1/messages/{id}?peer=` | takes a message this account sent back, on both sides |
| `POST /v1/reactions` | `{"peer": "...", "messageId": "...", "emoji": "👍"}`; an empty emoji takes it back |
| `POST /v1/receipts` | `{"peer": "...", "messageId": "..."}` tells the peer their message was read, and every earlier one with it |
| `POST /v1/chats/{fingerprint}/clear` | asks the peer to drop the conversation, and drops this side of it |
| `GET /v1/events?since=&wait=` | everything that happened after `since`, waiting up to `wait` seconds |

A group account answers `GET /v1/status` and refuses every other route with
`409`: it relays its members itself, and nothing else may write in its name.

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

A bot that sends notices rather than answers has one thing to watch, and it is
visible here.

**Admission.** A message is admitted to a mailbox by the **pass** its owner
issued this account when they accepted the contact. A pass is one per
correspondent and nothing runs it down, so there is no stash to keep topped up
and no count to read: `GET /v1/contacts` answers `writable` and that is the whole
of it.

```json
[{"fingerprint": "...", "name": "alice", "pending": false, "accepting": false, "writable": true}]
```

`writable` is false while a contact request has not been accepted, and after the
correspondent revokes what they issued - at which point sends to them fail and
keep failing, because only they can admit this account again.

**Contact requests.** A request arrives as a `message` event whose line has the type
`contact.request`. `POST /v1/contacts` with `accept` answers it, and
`DELETE /v1/contacts/{fingerprint}` declines it. A bot accepts
requests automatically as they arrive unless that is switched off in the panel.

**Delivery.** `POST /v1/messages` answers as soon as the mailbox takes the
message, and `confirmed` says only whether the far side signed for it inside the
send's own wait. What became of it afterwards is on the line and in the feed:

```json
{"id": 12, "messageId": "5373671a07d8b517", "outgoing": true, "text": "notice",
 "status": "delivered"}
```

`status` is `sending` while the courier is working, `delivered` once the
recipient's server has signed for it, `read` once the recipient's read receipt
covers it, or `failed: <reason>`. A change to `delivered` or `failed` is an event
of kind `status` carrying the same text, and a read one is an event of kind
`receipt`, so a long poll sees both without re-reading the conversation.

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

## What the desktop client draws in a message

A body is plain text on the wire, and the daemon sends it exactly as given. The
desktop client (`client-qt`) reads a few markers in it when it draws the bubble,
and a bot writing for that audience can use them:

| Written in the body | Drawn there |
|---|---|
| `*text*`, `**text**`, `~~text~~` | italic, bold, struck through |
| `!!text!!` | a pressable word; pressing it sends `text` back as an ordinary message |
| `!name` | a pressable name that opens their add-a-contact form, filled in |
| `http://...`, `https://...` | an address, opened only after a warning about what following it discloses |

`!!/help!!` is therefore a command offered inside a sentence: the press arrives
here as an ordinary incoming message whose text is `/help`, indistinguishable
from the user having typed it. An inline keyboard is the other way of offering
one - it arrives as `bot.callback` carrying a reference to the message the button
was attached to, and the press leaves nothing in the conversation.

Nothing between `!!` and `!!` is read as markup, so what is written there is what
comes back. A body meant to be shown as typed should avoid `*`, `~~` and `!!`;
single markers that do not pair off are drawn as themselves.

`client-qt/docs/Formatting.md` specifies the whole of it. The daemon's own web
interface does not draw any of it yet.
