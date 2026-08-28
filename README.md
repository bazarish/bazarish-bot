# Bazarish client daemon

A Bazarish account that runs as a service instead of as a desktop application,
and answers a REST API. Automation talks to that API and never to the protocol:
a bot is written against `POST /v1/messages`, not against sealed envelopes,
tokens and destinations.

It is self-hosting. The daemon carries its own mailbox and federates on its own
destination, so a bot depends on no one else's server for its uptime - which is
the whole point of running one.

| Face | For |
|---|---|
| REST API | the automation the operator writes |
| Web interface | the operator: setup, contacts, avatar, and the chats themselves |

Both are served over loopback by default. Everything the daemon needs is in one
document; it takes the path to that document and nothing else:

```
bazarish-daemon --config /etc/bazarish/daemon.json
```

The protocol is specified in the `docs-main` repository. The client core this
daemon is built on lives in `common-cpp` and is shared with the desktop client
(`client-qt6`), so both speak the protocol through the same code.
