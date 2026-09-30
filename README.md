# Bazarish client daemon

A Bazarish account that runs as a service instead of as a desktop application,
and answers a REST API. Automation talks to that API and never to the protocol:
a bot is written against `POST /v1/messages`, not against sealed envelopes,
delivery passes and destinations.

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

## Installing

Debian 13 is the platform this is built and tested on, and the only one it is
offered for. Nothing here is Debian-specific on purpose - it is simply the one
that is tried.

```
apt install build-essential cmake git libssl-dev zlib1g-dev libboost-all-dev \
            libsqlcipher-dev libqrencode-dev libopus-dev
git clone --recursive https://github.com/bazarish/client-daemon
cmake -S client-daemon -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The embedded router is always built in; a daemon that would rather borrow one
over SAM - which is what several accounts on one host want - says so in its
config, not at build time.

Then lay it out, write the one config file, and start it:

```
install -D build/bazarish-daemon /opt/bazarish/client-daemon/bazarish-daemon
cp -r client-daemon/web /opt/bazarish/client-daemon/
install -D client-daemon/config.example.json /etc/bazarish/daemon.json
install -D client-daemon/packaging/bazarish-daemon.service \
        /etc/systemd/system/bazarish-daemon.service
adduser --system --group --home /var/lib/bazarish bazarish
systemctl enable --now bazarish-daemon
```

Open the address it answers on. The first visitor sets the password, and
everything else - the transport, the account, the tokens - is set there.

## What it will not do

- **It is not multi-device.** One daemon serves one account and is that
  account's server. An account can be exported and imported to move it to
  another daemon; two daemons cannot serve one account, and the interface says
  so where it offers the export.
- **It is not a desktop client.** No calls, no file picker. What a bot and its
  operator need, and no more.

## Documents

| Document | Subject |
|---|---|
| [docs/Api.md](docs/Api.md) | The REST API a bot is written against |
| [docs/Design.md](docs/Design.md) | How the daemon is put together and why |
| [TODO.md](TODO.md) | Known gaps, worth reading before the next change |
