# What this daemon is, and why it is shaped this way

## The job

An operator writes automation. They should not have to learn sealed envelopes,
prepaid tokens, destinations or leasesets to do it. So the daemon holds one
Bazarish account and offers two faces over it: a REST API for the automation,
and a web interface for the human who runs it.

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

This has one consequence worth stating, because it removes machinery rather than
adding it. On a public server the account's serving destination is delegated:
the user keeps the identity key and hands the server a short-lived offline
transient, so an operator who is not the user cannot become them. Inside one
process there is no such boundary - the account, the mailbox and the operator
are the same party - so there is nothing to delegate. The daemon owns its
destination key outright.

Two things follow from that, and both are welcome:

- No offline keys, so the destination can publish an encrypted leaseset (b33)
  like any other, and the address is not forced back to a plain b32.
- No offline keys, so the SAM transport is enough. The daemon can speak to a
  router that is already running instead of carrying one, which is what makes
  many accounts on one host cheap: many processes, one router.

The embedded router is still built in by default, so a single daemon is a single
thing to install. `-DBAZARISH_WITH_I2PD=OFF` drops it and leaves a SAM-only
build.

## What it is not

- **Not multi-device.** One account, one process, one place. The desktop client
  synchronises devices; a service does not need to and paying for it here would
  buy nothing. An account is moved by the same password-protected `.bazarish`
  backup the desktop client writes - the same file restores it in either - and
  moving is not sharing: two daemons cannot serve one account.
- **Not a second protocol.** Where the REST API names a thing, it names the
  thing the protocol already has.

## The API

REST, and events by long poll: the same shape as the rest of the fleet, callable
from any language with an HTTP client and readable in `curl`. The daemon holds a
poll open and pushes when something happens, so automation neither spins nor
lags.

Authentication is a token from the config file. Whoever holds it holds the
account, so the listener is on loopback unless the operator moves it
deliberately, and the daemon refuses to start without a token rather than
serving an open account.

## Where the code comes from

| From | What |
|---|---|
| `common-cpp` | the client core (accounts, session, courier, invites, bot framework) and the crypto and I2P transport under it |
| `bazarish-server` | `server-core`: the mailbox, the delivery engine and the federation this daemon serves to itself |

The client core is shared with the desktop client rather than copied, so both
front-ends speak the protocol through exactly the same code.
