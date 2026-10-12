# Bazarish bot

Bazarish accounts that run as a service instead of as a desktop application,
and answer a REST API. Automation talks to that API and never to the protocol:
a bot is written against `POST /v1/messages`, not against sealed envelopes,
delivery passes and destinations.

One daemon holds any number of accounts on one I2P router, like the desktop
client does. Each account is a **bot** or a **group**; the two differ in how
they send (see [docs/Design.md](docs/Design.md)).

It is self-hosting. The daemon carries the accounts' mailboxes and federates on
each account's own destination, so a bot depends on no one else's server for its
uptime - which is the whole point of running one.

| Face | For |
|---|---|
| REST API | the automation the operator writes |
| Web interface | the operator: the accounts, their contacts and tokens, moving them in and out, and the chats themselves |

Both are served over loopback by default. Everything the daemon needs is in one
document; it takes the path to that document and nothing else:

```
bazarish-bot --config /etc/bazarish/bot.json
```

The protocol is specified in the `docs-main` repository. The client core this
daemon is built on lives in `common-cpp` and is shared with the desktop client
(`client-qt`), so both speak the protocol through the same code.

## Installing

Debian 13 is the platform this is built and tested on, and the only one it is
offered for. Nothing here is Debian-specific on purpose - it is simply the one
that is tried.

```
apt install build-essential cmake git libssl-dev zlib1g-dev libboost-all-dev \
            libsqlcipher-dev libqrencode-dev libopus-dev
git clone --recursive https://github.com/bazarish/bazarish-bot
cmake -S bazarish-bot -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The embedded router is always built in and carries every account in the
daemon; a daemon that would rather borrow a router already running on the host
over SAM says so in its interface, not at build time.

Then lay it out, write the one config file, and start it:

```
install -D build/bazarish-bot /opt/bazarish/bazarish-bot/bazarish-bot
cp -r bazarish-bot/web /opt/bazarish/bazarish-bot/
install -D bazarish-bot/config.example.json /etc/bazarish/bot.json
install -D bazarish-bot/packaging/bazarish-bot.service \
        /etc/systemd/system/bazarish-bot.service
adduser --system --group --home /var/lib/bazarish bazarish
systemctl enable --now bazarish-bot
```

Open the address it answers on. The first visitor sets the password, and
everything else - the transport, the accounts, the tokens - is set there.

## What it will not do

- **It is not multi-device.** The daemon is the server of every account it
  holds. An account comes in and goes out the way the desktop client moves one:
  a `.bazarish` backup, or pairing over I2P with a link and a code. Either moves
  the account; two places cannot serve one account, and the interface says so
  where it offers the export.
- **It is not a desktop client.** No calls, no file picker. What a bot and its
  operator need, and no more.

## Documents

| Document | Subject |
|---|---|
| [docs/Api.md](docs/Api.md) | The REST API a bot is written against |
| [docs/Design.md](docs/Design.md) | How the daemon is put together and why |
| [TODO.md](TODO.md) | Known gaps, worth reading before the next change |
