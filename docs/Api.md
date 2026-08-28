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
| `GET /v1/contacts` | the contacts, each with `pending` for a request not yet accepted |
| `POST /v1/contacts` | `{"invite": "...", "text": "..."}` adds one; `{"accept": "<fingerprint>"}` accepts a request |
| `GET /v1/chats` | the last line of every conversation |
| `GET /v1/messages?peer=&before=&limit=` | a conversation, newest first |
| `POST /v1/messages` | `{"peer": "...", "text": "...", "keyboard": [...]}` |
| `GET /v1/events?since=&wait=` | everything written after `since`, waiting up to `wait` seconds |

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

An event is a line of a conversation:

```json
{"id": 42, "peer": "<fingerprint>", "outgoing": false, "type": "text",
 "text": "hello", "messageId": "4054cd273aa34b4b", "sentAt": 1787932486147}
```

`type` is the end-to-end content type - `text`, `contact.request`, `bot.command`,
`bot.callback`, `file`, `picture`, `voice`. A bot answering commands watches for
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
