# What this daemon is, and why it is shaped this way

## The job

An operator writes automation. They should not have to learn sealed envelopes,
delivery passes, destinations or leasesets to do it. So the daemon holds one
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

This has one consequence worth stating. On a public server the account's serving
destination is delegated: the user keeps the master key and hands the server a
short-lived offline transient, so an operator who is not the user cannot become
them. Inside one process there is no such boundary - the account, the mailbox and
the operator are the same party - but the delegation stays, because it is not
only a trust boundary. The per-day blinded keys it carries are what let the
server publish the address as an **encrypted** leaseset without holding the
master signing key, and the server refuses a delegation that has none: a bare
master would build tunnels and then never publish.

So the daemon keeps its master on disk and issues a delegation to its own server
side, re-issuing it from that master well before the term runs out. Two things
follow:

- The address is the blinded b33 of an encrypted leaseset, like every other
  destination in the project. Nothing is forced back to a plain b32.
- The SAM transport asks an external router for the same encrypted leaseset, so
  borrowing a router stays possible - which is what makes many accounts on one
  host cheap: many processes, one router. It needs i2pd >= 2.61.0, and the one
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

Authentication is a bearer token, issued in the web interface under a name and
shown once. Whoever holds one holds the account, so the listener is on loopback
unless the operator moves it deliberately, and the first visitor to the web
interface sets the password that guards the issuing.

## Where the code comes from

| From | What |
|---|---|
| `common-cpp` | the client core (accounts, session, courier, invites, bot framework) and the crypto and I2P transport under it |
| `bazarish-server` | `server-core` for the mailbox, the delivery engine and the federation this daemon serves to itself, and `service-node` for the account it registers with itself |

The client core is shared with the desktop client rather than copied, so both
front-ends speak the protocol through exactly the same code.
