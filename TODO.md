# Backlog

Work that is known to be missing, kept here so it is found before the next
change rather than after it.

## The delegated address has not been run over SAM

The server side operates this daemon's address under a delegation it issues to
itself, because the per-day blinded keys in one are what let an encrypted leaseset
be published at all. That path is live-verified with the **embedded** router: two
daemons federated a full conversation over real I2P.

Borrowing a router over SAM is meant to work the same way - the backend asks for
the same encrypted leaseset type and derives the same b33 - but it has not been
run. Two things to watch when it is: that an external i2pd (>= 2.61.0) publishes
the blinded leaseset from the delegation rather than refusing it, and that a
re-issue, which rebuilds the SAM session rather than swapping the transient in
place, does not drop the account's streams for longer than a poll.

## Draw message formatting in the web interface

The desktop client draws light markup in a message body: `*italic*`, `**bold**`,
`~~struck~~`, `!!text!!` (pressable, sends `text` back), `!name` (pressable,
opens the add-a-contact form) and `http(s)` addresses behind a warning about what
following one discloses. The grammar and the rules are specified in
`client-qt6/docs/Formatting.md`, and `docs/Api.md` says what a bot may write.

The daemon's web interface draws none of it. A conversation with a bot that
writes for the desktop therefore reads as `**bold**` and `!!/help!!` in the one
place an operator watches that bot from.

What it needs:

- a parser mirroring `client-qt6/app/Markup.cpp` - the same rules, or the two
  faces will disagree about what a body means;
- DOM built node by node, never `innerHTML`: the body is a correspondent's text,
  and a page that accepted their tags would fetch what they named in one, from
  the operator's browser (`app.js` sets `textContent` today, which is why it is
  safe as it stands);
- the press on `!!text!!` sending it through `POST /v1/messages`, and the same
  warning before an address is opened;
- `!name` has nowhere to go here: the web interface has no add-a-contact form
  filled in from a message. Either leave those as text or give it one.
