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

Every account is one of two kinds, set when it is added and changeable later.
They differ only in how they send.

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
  i2pd >= 2.61.0, and the one
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

## Where the code comes from

| From | What |
|---|---|
| `common-cpp` | the client core (accounts, session, courier, invites) and the crypto and I2P transport under it |
| `bazarish-server` | `server-core` for the mailbox, the delivery engine and the federation this daemon serves to itself, and `service-node` for the account it registers with itself |

The client core is shared with the desktop client rather than copied, so both
front-ends speak the protocol through exactly the same code.
