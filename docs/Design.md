# What this daemon is, and why it is shaped this way

## The job

An operator writes automation. They should not have to learn sealed envelopes,
delivery passes, destinations or leasesets to do it. So the daemon holds
Bazarish accounts and offers two faces over them: a REST API for the automation,
and a web interface for the human who runs it.

## Many accounts, one router

The daemon is shaped like the desktop client: one I2P router for the process,
and as many accounts on it as the operator adds. One server side - mailbox,
account node, facade - serves all of them, and each account still federates on
a destination of its own, so the accounts here are no easier to tie together
than accounts on any other server. An API token opens the one account it was
issued for; the web interface is the operator's and sees every account.

## Bot and group

Every account is one of two kinds, chosen when it is added and fixed from then
on: a group with members cannot meaningfully become a bot again.

- A **bot** sends the way every client does: from an outbound address held for
  that one correspondent, never reused for another.
- A **group** sends `content` to all its members from one outbound address. A
  group relays every post to every member, so building and holding an address
  per member is what limits it; with one address the cost is paid once. The
  price is that a server holding several members sees one sender in their
  mailboxes and learns they share a group, though not which. The protocol
  allows this for a group host and nobody else (`docs-main`,
  `api/Federation.md`).

Contact requests leave from an address of their own in both kinds. The shared
address lives by the same rule as any other: an hour at most, and closed after
ten minutes without a delivery.

## The group host

A group account runs the protocol of `docs-main/Groups.md` itself; nothing of
it goes through the REST API, which refuses a group account outright. Its
state - settings, members, posts, the relay log, reactions, requests - lives in
a database of its own, `groups/<id>.db`, encrypted like the transcripts and
apart from them: a post is relayed under its author's id to everyone, which the
transcript's one-id-one-row rule would not hold.

- **Relaying is a log.** Every post, edit, delete and change of reaction counts
  is a row; every member has a cursor into it. A pump sends each member the next
  row once the previous one is stored, so a post always arrives before its edit,
  and the log survives a restart: a post to a thousand members that takes an
  hour is not lost by stopping halfway. A refusal from the member's server
  (`DELIVERY_REJECTED`) means the member left: the name stays theirs and the
  pump stops; anything else is tried again a minute later.
- **The host's own posts.** A post from the panel or `POST /v1/posts` is a row
  with no author: it goes to every member, admins included, marked `fromHost`,
  so a channel needs no member account behind it.
- **History is a cursor.** A newcomer's cursor starts N posts back, so the last
  N arrive as ordinary relays, built from each post's current state.
- **Names** are checked with ICU: case folding, NFKC, and the UTS #39 confusable
  skeleton as the uniqueness key, with names mixing scripts refused. A name
  stays with its member until they change it from Settings.
- **Locks.** The block is filled in under the account's session lock, so the
  order is the session first, then the engine, never the reverse: the engine
  decides under its own lock and sends after releasing it.
- **One device.** A group cannot be paired to a second device, and the backup
  carries the account alone: members, posts and settings stay on this host.

The chat texts the host sends come in the group's own language, one of five,
chosen by the owner, who may also replace the greeting and the welcome with
texts of their own.

Everything that makes a conversation a *bot* conversation - commands, inline
keyboards, callbacks - is end-to-end content and nothing else. The
infrastructure never learns that a bot was involved, and this daemon needs no
protocol of its own: it is an ordinary identity, exactly as the protocol
specifies a bot to be.

## Self-hosting

The daemon carries its own mailbox and federates on its own destination. A bot
that runs as a service already has the uptime a mailbox needs, and depending on
somebody else's server for it buys nothing while adding an operator, a
subscription and a second thing to be down.

This has one consequence worth stating. On a public server the account's serving
destination is delegated: the user keeps the master key and hands the server a
short-lived offline transient, so an operator who is not the user cannot become
them. Inside one process there is no such boundary - the account, the mailbox and
the operator are the same party - but the delegation stays, because it is not
only a trust boundary. The per-day blinded keys it carries are what let the
server publish the address as an **encrypted** leaseset without holding the
master signing key, and the server refuses a delegation that has none: a bare
master would build tunnels and then never publish.

So the daemon keeps each account's master on disk and issues a delegation to its
own server side, re-issuing it from that master well before the term runs out.
Two things follow:

- The address is the blinded b33 of an encrypted leaseset, like every other
  destination in the project. Nothing is forced back to a plain b32.
- The SAM transport asks an external router for the same encrypted leaseset, so
  borrowing a router already running on the host stays possible. It needs
  i2pd >= 3.0.0, and the one
  thing SAM will not do is swap a live transient in place: a re-issue there
  rebuilds the session, which keeps the address because the master is unchanged.

Both of those have been run against an external i2pd, not only reasoned about:
two daemons on one borrowed router published b33 addresses from their delegations,
held a conversation in both directions, and kept their addresses across a re-issue.
One thing to know when borrowing a router: a destination there gets exactly one
stream forward, and asking twice takes down the one that works.

The embedded router is always built in, so a single daemon is a single thing to
install. Borrowing a router instead is a configuration, not a build: the
transport is chosen at runtime, and there is one shape of the library.

## What it is not

- **Not multi-device.** Each account lives in one place and runs on its own. The
  desktop client keeps its devices in step through the account's own mailbox -
  a copy of everything sent, the address book, read marks, the `device.*`
  messages; the daemon switches all of that off in the client core
  (`Session::setDeviceSync(false)`), so nothing it does lands in its own
  mailbox and nothing asks for devices that do not exist. A service does not
  need another device, and paying for one here would buy nothing. An account is
  moved the two ways the desktop client moves one:
  the password-protected `.bazarish` backup - the same file restores it in
  either - or pairing, which carries that same bundle over I2P, sealed under a
  short code, to whoever opens the link. Moving is not sharing: two places
  cannot serve one account.
- **Not a second protocol.** Where the REST API names a thing, it names the
  thing the protocol already has.

## The API

REST, and events by long poll: the same shape as the rest of the fleet, callable
from any language with an HTTP client and readable in `curl`. The daemon holds a
poll open and pushes when something happens, so automation neither spins nor
lags.

Mail reaches the account the way it reaches every client: the account keeps one
`events` request open to its own mailbox and reads the mailbox when that request
says something is waiting, and once when the account comes up. What it reads goes
into the transcript, and a bot's waiting poll is answered from there at once.
There is no periodic mailbox read and no fallback; a wait that fails is asked
again. Upkeep - renewing the delegation, the routing fan-out after a new
address, the alias status, re-issuing the card - runs on its own five-second
turn and never reads the mailbox. Stopping the daemon waits for the open
request to come back, up to the 30 seconds it is held.

Authentication is a bearer token, issued in the web interface under a name and
shown once. Whoever holds one holds the account, so the listener is on loopback
unless the operator moves it deliberately.

The web interface is the operator's, and the operator is a key, not a password:
the interface shows a sign-in challenge, the operator signs it with their
Bazarish identity, and the daemon checks the signature against the `operator`
fingerprint in its config - the same sign-in-with-key the admin panel uses. A
host with no operator in its config is unclaimed, and the first key to sign in
claims it and is written there. There is one operator for every account and no
sign-in per account. Account files carry no passphrase either: the operator and
the server of an account are the same party on the same disk, so every account
opens by itself at start.

## Aliases and signing in elsewhere

An account here is a full identity, so it does what the desktop client does
with one. The web interface signs a sign-in-with-key challenge with the
account's own key, which is how the operator buys and renews an alias for a bot
in the alias registry, or signs in as that bot to anything else that speaks the
same sign-in. The key never leaves the daemon: the challenge comes in, the
signature goes out. The API has no such route, because a token is for one
bot's conversations and not for speaking as it elsewhere.

The aliases are serviced the way the desktop client services them: the
account asks the registry what it holds when it comes up, once a minute until
the registry has answered, and after that whenever something is due. It points
the aliases it wants pointing at its current address, and repoints them after
a new address. The ask runs off the account's lock, so a registry that does not
answer holds up nothing else; stopping the daemon waits for an ask in flight to
come back. A contact is added by alias as well as by invite.

## Backing up the whole host

The System page exports one zip of the state directory and the config file,
with a `bazarish-bot-backup.json` manifest naming the format and the version.
Entries are stored, not deflated, and keep their file modes. While it is
written every account is closed, so no account database is copied mid-write;
the server stays up, since its storage replaces each file whole.

An import is uploaded in pieces into `.restore-upload.zip` in the state
directory and checked as a whole: the manifest, the checksums, and that every
entry lands inside the state directory. The daemon then stops, replaces the
state directory with the archive's, and exits with code 90 for systemd to start
it again. The config file stays as it is: it names this host's address and
administrator.

## Where the code comes from

| From | What |
|---|---|
| `common-cpp` | the client core (accounts, session, courier, invites) and the crypto and I2P transport under it |
| `bazarish-server` | `server-core` for the mailbox, the delivery engine and the federation this daemon serves to itself, and `service-node` for the account it registers with itself |

The client core is shared with the desktop client rather than copied, so both
front-ends speak the protocol through exactly the same code.
