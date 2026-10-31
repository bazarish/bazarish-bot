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
| `GET /v1/status` | the account's `fingerprint`, `name`, `kind` (`bot` or `group`), `destination`, `status` and the id of the last event |
| `GET /v1/invite` | the invite URI to hand out to be added as a contact |
| `GET /v1/contacts` | the contacts, each with `pending` for a request not yet accepted, `accepting` while the acceptance is on its way, and `writable` for whether this account holds the pass that admits it to their mailbox |
| `POST /v1/contacts` | `{"invite": "...", "text": "..."}` or `{"alias": "!name", "text": "..."}` adds one; `{"accept": "<fingerprint>"}` accepts a request |
| `DELETE /v1/contacts/{fingerprint}` | removes a contact and revokes the pass it was issued, or declines a request; the conversation stays |
| `GET /v1/chats` | the last line of every conversation |
| `GET /v1/messages?peer=&before=&limit=` | a conversation, newest first |
| `POST /v1/messages` | `{"peer": "...", "text": "...", "replyTo": "<messageId>", "keyboard": [...]}`; `replyTo` and `keyboard` are optional, and a message with a keyboard takes no `replyTo` |
| `POST /v1/pictures` | `{"peer": "...", "data": "<base64>", "name": "qr.png", "replyTo": "<messageId>"}` sends a picture; `name` and `replyTo` are optional |
| `POST /v1/files` | `{"peer": "...", "path": "/srv/outbox/report.pdf", "replyTo": "<messageId>"}` offers a file on this host; `replyTo` is optional |
| `POST /v1/downloads` | `{"peer": "...", "messageId": "...", "path": "/srv/inbox/report.pdf"}` fetches a file the peer offered into a path on this host |
| `GET /v1/transfers` | every file transfer since the account opened, each as `GET /v1/transfers/{messageId}` shows it |
| `GET /v1/transfers/{messageId}?peer=` | how far one transfer has got |
| `DELETE /v1/transfers/{messageId}?peer=` | stops a transfer under way |
| `POST /v1/menu` | `{"peer": "...", "items": [{"title": "Balance", "command": "balance"}]}` sets the commands this account offers that contact; `[]` takes them away |
| `POST /v1/forward` | `{"from": "<fingerprint>", "messageId": "...", "to": "<fingerprint>"}` sends a text message on, marked forwarded |
| `POST /v1/messages/{id}` | `{"peer": "...", "text": "..."}` revises a message this account sent |
| `DELETE /v1/messages/{id}?peer=` | takes a message this account sent back, on both sides |
| `POST /v1/reactions` | `{"peer": "...", "messageId": "...", "emoji": "👍"}`; an empty emoji takes it back |
| `POST /v1/receipts` | `{"peer": "...", "messageId": "..."}` tells the peer their message was read, and every earlier one with it |
| `POST /v1/chats/{fingerprint}/clear` | asks the peer to drop the conversation, and drops this side of it |
| `GET /v1/events?since=&wait=` | everything that happened after `since`, waiting up to `wait` seconds, at most 60 |
| `POST /v1/posts` | `{"text": "..."}` posts in a group as the group itself; group accounts only |

A group account answers `GET /v1/status` and `POST /v1/posts` and refuses every
other route with `409`: its members' posts it relays itself.

### Posting in a group

```bash
curl -s -X POST http://127.0.0.1:8440/v1/posts \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"text": "**Tonight** at eight."}'
```

```json
{"id": "5373671a07d8b517"}
```

The post goes to every member, admins included, and members see it with no
author above it: the group itself is speaking. It takes the markup below. A
post that does not fit one message is refused with `400`. The panel's group
posts tab writes through the same path, with a preview.

### Sending

```bash
curl -s -X POST http://127.0.0.1:8440/v1/messages \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "text": "hello"}'
```

```json
{"messageId": "5373671a07d8b517"}
```

An inline keyboard is rows of buttons, each with a `text` and either a `data`
(the peer's tap sends a `bot.callback` back) or a `command`:

```json
{"peer": "...", "text": "Pick one:",
 "keyboard": [[{"text": "Ping", "data": "ping"}, {"text": "Help", "command": "help"}]]}
```

### Sending a picture

```bash
curl -s -X POST http://127.0.0.1:8440/v1/pictures \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"peer\": \"<fingerprint>\", \"name\": \"qr.png\", \"data\": \"$(base64 -w0 qr.png)\"}"
```

```json
{"messageId": "a1c3f0e29b4d7765"}
```

A picture travels inside the message, so it has a limit: the decoded bytes are at
most **256 KiB**, the same cap the client's composer shrinks a photo to, which
keeps the picture and its envelope inside the 512 KiB message. The daemon does
not shrink or re-encode anything; a larger picture is refused with `413`.

Only PNG and JPEG are taken, told by their first bytes rather than by any field
of the request; anything else is refused with `415`, and `data` that is not
base64 with `400`. `name` is kept when it is a plain file name of at most 128
bytes, and becomes `picture.png` or `picture.jpg` otherwise. The line it leaves
in the conversation has the type `image` and the name as its text.

### Files

A file does not travel in a message. The message announces it - name, size, type and
SHA-256 - and the bytes go from one side to the other over a one-time I2P address
when the receiver asks for them, so a file of any size is sent by naming it:

```bash
curl -s -X POST http://127.0.0.1:8440/v1/files \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "path": "/srv/outbox/report.pdf"}'
```

```json
{"messageId": "9f1c0a77e2d34b58"}
```

The path is absolute and names a regular file on this host; otherwise the answer is
`400` or `404`. The daemon reads the file once to announce it and again each time the
peer asks for it, so it stays at that path, unchanged, until the peer has it.

A file that comes in is a line of the type `file`:

```json
{"id": 7, "messageId": "4c2e81d07a5b9f13", "outgoing": false, "type": "file",
 "text": "report.pdf", "file": {"name": "report.pdf", "size": 3145728,
 "mime": "application/pdf", "sha256": "5d41..."}}
```

`mime` and `sha256` are what the sender announced. Nothing is fetched until it is asked
for:

```bash
curl -s -X POST http://127.0.0.1:8440/v1/downloads \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "messageId": "4c2e81d07a5b9f13", "path": "/srv/inbox/report.pdf"}'
```

The path is absolute, its directory exists and nothing is there yet: what is already
at a path is never written over, and the answer is `409`. The bytes gather in
`<path>.part`, and the file appears at the path once it is whole and its SHA-256 is the
announced one; the line's `file` then carries that `path`. A download already under way
for the message is also `409`.

```bash
curl -s "http://127.0.0.1:8440/v1/transfers/4c2e81d07a5b9f13?peer=<fingerprint>" \
  -H "Authorization: Bearer $TOKEN"
```

```json
{"peer": "...", "messageId": "4c2e81d07a5b9f13", "direction": "receive", "state": "running",
 "stage": "receiving", "bytes": 1048576, "total": 3145728, "path": "/srv/inbox/report.pdf"}
```

`state` is `requested`, `running`, `done` or `failed`, the last with an `error`; `stage`
names the step it is at. `bytes` and `total` count what crosses the network: the file
encrypted, which is 1 to 16 bytes longer than its `size`. Each change of `state` is an event of the kind `transfer`
carrying the new state, so a long poll waits for `done` without asking. A file this
account sent shows the same while the peer fetches it, with the `direction` `send`.
Transfers are remembered from the moment the account opens.

Under the packaged unit the daemon does not see `/home` and writes only under
`/var/lib/bazarish` and `/etc/bazarish`; a drop-in with `ReadWritePaths=` opens another
directory for downloads.

### The menu

```bash
curl -s -X POST http://127.0.0.1:8440/v1/menu \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"peer": "<fingerprint>", "items": [{"title": "Balance", "command": "balance"}]}'
```

```json
{"sent": true}
```

A menu is the list of commands a contact's client shows above the chat; a press
sends the command as a `/name` line. The daemon keeps one menu per contact and
sends it under a fresh random id when the items change; the same items again
answer `"sent": false` and send nothing. The limits are the protocol's
(`docs-main` `Bots.md`): at most 32 items, a title of 1 to 32 characters of plain
text, a command of 1 to 32 of `a-z`, `0-9` and `_` with no slash. Anything else
is refused with `400`.

Every message a contact writes names the menu it holds. When a message names none
or another one, the daemon sends the menu again at once, before the message
reaches the event stream, and logs it; a bot does nothing for that.

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
 "messageId": "4054cd273aa34b4b", "text": "hello", "actor": "<fingerprint>",
 "sentAt": 1787932486147}
```

For a `message`, `actor` is the peer when the line came from them; a line this account
sent has no `actor`.

`kind` is one of `message`, `edit`, `delete`, `reaction`, `receipt`, `status`,
`transfer` or `chat-cleared`. Only a `message` is a new line; the others are about a line that is already there,
so a bot that keeps its own copy applies them to it. Read the conversation back
with `GET /v1/messages` and each line carries its reactions and whether it has
been edited:

```json
{"id": 5, "messageId": "b69ccb66654dd3fb", "outgoing": false, "type": "text",
 "text": "123", "edited": false, "reactions": [{"reactor": "me", "emoji": "👍"}]}
```

A reaction's `reactor` is `me` for this account's own, and the peer's
fingerprint for theirs.

A line's `type` is the end-to-end content type - `text`, `image`, `audio`,
`file`, `contact.request`, `bot.command`, `bot.callback`. A bot answering commands watches for
`bot.command` and for text beginning with a slash, which the protocol says are
two spellings of one thing.

## A bot in ten lines

```bash
TOKEN=...; SINCE=0
while :; do
  ANSWER=$(curl -s "http://127.0.0.1:8440/v1/events?since=$SINCE&wait=60" \
           -H "Authorization: Bearer $TOKEN")
  SINCE=$(echo "$ANSWER" | jq -r .lastEventId)
  echo "$ANSWER" | jq -c '.events[] | select(.kind == "message" and .actor == .peer)
                          | {peer, text: ("you said: " + .text)}' |
  while read -r REPLY; do
    curl -s -X POST http://127.0.0.1:8440/v1/messages \
      -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
      -d "$REPLY" > /dev/null
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

**Delivery.** `POST /v1/messages` answers as soon as the message is queued; this
account delivers it in the background with retries, as the desktop client does.
What became of it is on the line and in the feed:

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
| ```` ```text``` ```` | a block kept exactly as typed; pressing it copies it |
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

`client-qt/docs/Formatting.md` specifies the whole of it. The panel draws the
same in its conversations, where the pressable forms work as they do in the
client, and in a group's posts and the preview of a new one, where they do not.
