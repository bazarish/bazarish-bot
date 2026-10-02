# Backlog

Work that is known to be missing, kept here so it is found before the next
change rather than after it.

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
