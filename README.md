# Bazarish bot

Bazarish accounts that run as a service instead of as a desktop application,
and answer a REST API. Automation talks to that API and never to the protocol:
a bot is written against `POST /v1/messages`, not against sealed envelopes,
delivery passes and destinations.

One daemon holds any number of accounts on one I2P router, like the desktop
client does. Each account is a **bot** or a **group**. A group account is a
group or channel host (`docs-main/Groups.md`), run from the web interface; a
bot account is driven by the REST API (see [docs/Design.md](docs/Design.md)).

It is self-hosting. The daemon carries the accounts' mailboxes and federates on
each account's own destination, so a bot depends on no one else's server for its
uptime - which is the whole point of running one.

| Face | For |
|---|---|
| REST API | the automation the operator writes |
| Web interface | the operator: the accounts, their contacts, aliases and tokens, signing in elsewhere as one of them, moving them in and out, and the chats themselves |

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
offered for. Every release carries a package for amd64 and arm64:

```
apt install ./bazarish-bot_<version>_<arch>.deb
```

The package owns the program and its unit and nothing else. On first install it
writes `/etc/bazarish/bot.json` from the template and creates the `bazarish`
user; an upgrade replaces the program, keeps the config and the state in
`/var/lib/bazarish`, and restarts the service, starting it if it was stopped.

To build it yourself:

```
apt install build-essential cmake ninja-build git libssl-dev zlib1g-dev \
            libboost-dev libboost-program-options-dev libsqlcipher-dev \
            libqrencode-dev libopus-dev libicu-dev dpkg-dev python3 pkg-config
git clone --recursive https://github.com/bazarish/bazarish-bot
cd bazarish-bot && packaging/deb/build.sh out
```

The embedded router is always built in and carries every account in the
daemon; a daemon that would rather borrow a router already running on the host
over SAM says so in its interface, not at build time.

The config listens on loopback; edit `listen` in `/etc/bazarish/bot.json` and
restart the service to open it elsewhere.

Open the address it answers on and sign the challenge it shows with your
Bazarish account's key in the desktop client. The first key to sign in
becomes the operator and is written into the config as `operator`; from then on
that key and no other opens the interface. Everything else - the transport, the
accounts, the tokens - is set there.

There is one operator for the whole host and no sign-in per account, and an
account file here carries no passphrase: the host is the account's server and
its operator, so a second lock on the same disk would guard nothing.

## What it will not do

- **It is not multi-device.** The daemon is the server of every account it
  holds and runs each one on its own, so it sends nothing to an account's own
  mailbox for other devices: no copies of what it sent, no address book, no
  read marks - none of the `device.*` traffic the desktop client keeps its
  devices in step with. An account comes in and goes out the way the desktop
  client moves one: a `.bazarish` backup, or pairing over I2P with a link and a
  code. Either moves the account; two places cannot serve one account, and the
  interface says so where it offers the export.
- **It is not a desktop client.** No calls, no file picker. What a bot and its
  operator need, and no more.

## Documents

| Document | Subject |
|---|---|
| [docs/Api.md](docs/Api.md) | The REST API a bot is written against |
| [docs/Design.md](docs/Design.md) | How the daemon is put together and why |
