// Bazarish project (c) 2026
"use strict";

const el = (id) => document.getElementById(id);
const content = () => el("content");

let peer = null;
let names = {};
let lastEventId = 0;
let watching = false;
let editing = null;

const kReactions = ["\u{1F44D}", "❤️", "\u{1F602}", "\u{1F389}", "\u{1F440}", "✅"];

async function call(method, path, body) {
    const options = {method, headers: {}};
    if (body !== undefined) {
        options.headers["Content-Type"] = "application/json";
        options.body = JSON.stringify(body);
    }
    const answer = await fetch(path, options);
    if (answer.status === 401) {
        location.href = "/";
        throw new Error("signed out");
    }
    const text = await answer.text();
    const parsed = text ? JSON.parse(text) : {};
    if (!answer.ok) {
        throw new Error(parsed.error || answer.statusText);
    }
    return parsed;
}

function node(tag, className, text) {
    const made = document.createElement(tag);
    if (className) {
        made.className = className;
    }
    if (text !== undefined) {
        made.textContent = text;
    }
    return made;
}

function decorateCopies(root) {
    const marks = window.bazarishCopy;
    if (!marks) {
        return;
    }
    for (const block of root.querySelectorAll("pre.copy:not([data-copyable])")) {
        block.dataset.copyable = "yes";
        block.parentNode.insertBefore(
            marks.button("Copy", () => block.textContent), block.nextSibling);
    }
    for (const value of root.querySelectorAll("[data-copy]:not([data-copyable])")) {
        value.dataset.copyable = "yes";
        value.append(marks.icon(() => value.dataset.copy, "Copy"));
    }
}

function facts(pairs) {
    const list = node("ul", "facts");
    for (const [label, value] of pairs) {
        const row = node("li");
        row.append(node("span", "fact-label", label));
        const held = node("b");
        const inner = node("span", "fact-value", value);
        inner.dataset.copy = value;
        held.append(inner);
        row.append(held);
        list.append(row);
    }
    return list;
}

function base64Of(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onerror = () => reject(new Error("that file could not be read"));
        reader.onload = () => resolve(String(reader.result).split(",")[1] || "");
        reader.readAsDataURL(file);
    });
}

function nameOf(fingerprint) {
    return names[fingerprint] || fingerprint.slice(0, 16) + "…";
}

function when(milliseconds) {
    return milliseconds ? new Date(milliseconds).toLocaleString() : "";
}

async function refreshRail() {
    const state = await call("GET", "/ui/state");
    const running = state.running;
    el("railDot").className = "dot " + (running ? "is-up" : "is-down");
    if (!running) {
        el("railName").textContent = state.hasAccount ? "account closed" : "no account yet";
        el("railTransport").textContent = state.status;
        return state;
    }
    const [account, settings] = await Promise.all([
        call("GET", "/ui/account"), call("GET", "/ui/settings")]);
    el("railName").textContent = account.name || "unnamed account";
    el("railTransport").textContent = settings.transport === "sam"
        ? "sam " + settings.samHost + ":" + settings.samPort
        : "own router";
    return state;
}

async function viewOverview() {
    const state = await call("GET", "/ui/state");
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Overview"));

    if (!state.hasAccount) {
        view.append(accountSetup());
        return view;
    }
    if (!state.running) {
        view.append(unlockCard(state));
        return view;
    }

    const account = await call("GET", "/ui/account");
    const cards = node("div", "cards is-stacked");

    const identity = node("article", "card");
    const head = node("div", "card-head");
    head.append(node("span", "dot is-up"));
    head.append(node("span", "card-name", account.name || "unnamed account"));
    identity.append(head);
    identity.append(facts([
        ["Fingerprint", account.fingerprint],
        ["Address", account.destination || "building…"],
    ]));
    cards.append(identity);

    const invite = node("article", "card");
    const inviteHead = node("div", "card-head");
    inviteHead.append(node("span", "card-name", "Invite"));
    invite.append(inviteHead);
    invite.append(node("p", "hint", "Hand this to whoever should be able to write to this account."));
    const line = node("pre", "copy", account.invite || "");
    invite.append(line);
    cards.append(invite);

    view.append(cards);
    return view;
}

function unlockCard(state) {
    const card = node("article", "panel-box");
    card.append(node("p", "view-title", "The account is closed"));
    card.append(node("p", "step-note",
        "It is encrypted at rest, so it waits for its passphrase at every start. "
        + "Status: " + state.status));
    const field = node("input", "field");
    field.type = "password";
    field.id = "unlockPassphrase";
    card.append(node("label", "field-label", "Passphrase"));
    card.append(field);
    const footer = node("p", "form-footer");
    const open = node("button", "btn", "Open");
    open.onclick = async () => {
        try {
            await call("POST", "/ui/account", {action: "open", passphrase: field.value});
            await render();
        } catch (error) {
            footer.append(node("span", "hint", error.message));
        }
    };
    footer.append(open);
    card.append(footer);
    return card;
}

function accountSetup() {
    const card = node("article", "panel-box");
    card.append(node("p", "view-title", "No account yet"));
    card.append(node("p", "step-note",
        "One account per daemon, and it is that account's own server. An account "
        + "here does not synchronise with a second device: there is nowhere to "
        + "synchronise from."));

    const name = node("input", "field");
    name.type = "text";
    name.placeholder = "what this account calls itself";
    card.append(node("label", "field-label", "Name"));
    card.append(name);

    const passphrase = node("input", "field");
    passphrase.type = "password";
    card.append(node("label", "field-label", "Passphrase for the account file (optional)"));
    card.append(passphrase);
    card.append(node("p", "hint",
        "Empty, and the daemon opens the account by itself at every start. Set one, "
        + "and it is encrypted at rest and unlocked here after every restart."));

    const bundle = node("input", "field");
    bundle.type = "file";
    bundle.accept = ".bazarish";
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";

    const footer = node("p", "form-footer");
    const create = node("button", "btn", "Create");
    const importing = node("button", "btn btn-quiet", "Restore a backup");
    footer.append(create, importing);
    card.append(footer);

    const importBlock = node("div", "block");
    importBlock.hidden = true;
    importBlock.append(node("p", "block-title", "Import"));
    importBlock.append(node("label", "field-label", "Backup file (.bazarish)"));
    importBlock.append(bundle);
    importBlock.append(node("label", "field-label", "The backup's password"));
    importBlock.append(bundlePassword);
    const importFooter = node("p", "form-footer");
    const doImport = node("button", "btn", "Import");
    importFooter.append(doImport);
    importBlock.append(importFooter);
    card.append(importBlock);

    const failure = node("p", "hint");
    card.append(failure);

    const start = async (body) => {
        try {
            await call("POST", "/ui/account", body);
            await render();
        } catch (error) {
            failure.textContent = error.message;
        }
    };
    create.onclick = () => start({action: "create", name: name.value,
        passphrase: passphrase.value});
    importing.onclick = () => { importBlock.hidden = false; };
    doImport.onclick = async () => {
        const file = bundle.files && bundle.files[0];
        if (!file) {
            failure.textContent = "Pick the bundle file first.";
            return;
        }
        await start({action: "import", bundle: await base64Of(file),
            bundlePassphrase: bundlePassword.value, passphrase: passphrase.value});
    };
    return card;
}

async function viewChats() {
    const view = node("section", "view");
    view.style.maxWidth = "none";
    view.append(node("h1", "view-title", "Chats"));

    const chat = node("div", "chat");
    const list = node("div", "chat-list");
    list.id = "chatList";
    const thread = node("div", "thread");
    thread.id = "thread";
    chat.append(list, thread);
    view.append(chat);
    return view;
}

async function drawChats() {
    const list = el("chatList");
    if (!list) {
        return;
    }
    const chats = await call("GET", "/ui/chats");
    list.textContent = "";
    if (chats.length === 0) {
        list.append(node("p", "empty", "No conversations yet."));
    }
    for (const chat of chats) {
        const item = node("button", "chat-item" + (chat.peer === peer ? " is-current" : ""));
        item.type = "button";
        item.append(node("span", "who", nameOf(chat.peer)));
        item.append(node("span", "last", previewOf(chat)));
        item.onclick = () => { peer = chat.peer; drawChats(); drawThread(); };
        list.append(item);
    }
    if (!peer && chats.length > 0) {
        peer = chats[0].peer;
        await drawChats();
        await drawThread();
    }
}

function previewOf(line) {
    if (line.type === "text" || line.type === "contact.request") {
        return (line.outgoing ? "you: " : "") + line.text;
    }
    return "[" + line.type + "]";
}

async function drawThread() {
    const thread = el("thread");
    if (!thread) {
        return;
    }
    thread.textContent = "";
    if (!peer) {
        thread.append(node("p", "empty", "Pick a conversation."));
        return;
    }

    const head = node("div", "thread-head");
    head.append(node("span", null, nameOf(peer)));
    thread.append(head);

    const log = node("div", "log");
    const lines = await call("GET", "/ui/messages?peer=" + encodeURIComponent(peer) + "&limit=100");
    for (const line of lines.slice().reverse()) {
        log.append(lineNode(line));
    }
    thread.append(log);

    const composer = node("form", "composer");
    const field = node("input", "field");
    field.type = "text";
    field.placeholder = editing ? "Edit the message" : "Write a message";
    field.autocomplete = "off";
    if (editing) {
        field.value = editing.text;
    }
    const send = node("button", "btn", editing ? "Save" : "Send");
    send.type = "submit";
    composer.append(field, send);
    composer.onsubmit = async (event) => {
        event.preventDefault();
        const text = field.value;
        if (!text) {
            return;
        }
        field.value = "";
        if (editing) {
            const target = editing;
            editing = null;
            await call("POST", "/ui/messages/" + target.messageId, {peer, text});
        } else {
            await call("POST", "/ui/messages", {peer, text});
        }
        await drawThread();
        await drawChats();
    };
    thread.append(composer);
    log.scrollTop = log.scrollHeight;
    field.focus();
}

function lineNode(line) {
    const isNote = line.type !== "text";
    const held = node("div", "line" + (line.outgoing ? " is-out" : "")
        + (isNote ? " is-note" : ""));

    const bubble = node("div", "bubble");
    bubble.textContent = isNote ? "[" + line.type + "] " + (line.text || "") : line.text;
    if (line.keyboard) {
        const keys = node("div", "keys");
        for (const row of line.keyboard) {
            for (const button of row) {
                keys.append(node("span", "key", button.text));
            }
        }
        bubble.append(keys);
    }
    held.append(bubble);

    if (line.reactions && line.reactions.length > 0) {
        const reactions = node("div", "reactions");
        for (const reaction of line.reactions) {
            const chip = node("span", "reaction" + (reaction.reactor === "me" ? " is-mine" : ""),
                reaction.emoji);
            chip.title = reaction.reactor === "me" ? "you" : nameOf(reaction.reactor);
            reactions.append(chip);
        }
        held.append(reactions);
    }

    const meta = node("div", "line-meta");
    meta.textContent = when(line.sentAt);
    if (line.edited) {
        meta.append(node("span", "edited", " · edited"));
    }
    held.append(meta);

    if (line.messageId) {
        held.append(toolsFor(line));
    }
    return held;
}

function toolsFor(line) {
    const tools = node("div", "line-tools");
    for (const emoji of kReactions) {
        const mine = (line.reactions || []).some(
            (reaction) => reaction.reactor === "me" && reaction.emoji === emoji);
        const button = node("button", "line-tool", emoji);
        button.type = "button";
        button.title = mine ? "take it back" : "react";
        button.onclick = async () => {
            await call("POST", "/ui/reactions",
                {peer, messageId: line.messageId, emoji: mine ? "" : emoji});
            await drawThread();
        };
        tools.append(button);
    }
    if (line.outgoing && line.type === "text") {
        const edit = node("button", "line-tool", "edit");
        edit.type = "button";
        edit.onclick = () => { editing = line; drawThread(); };
        const drop = node("button", "line-tool", "delete");
        drop.type = "button";
        drop.onclick = async () => {
            await call("DELETE", "/ui/messages/" + line.messageId
                + "?peer=" + encodeURIComponent(peer));
            await drawThread();
            await drawChats();
        };
        tools.append(edit, drop);
    }
    return tools;
}

async function viewContacts() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Contacts"));

    const rows = node("div", "rows");
    const contacts = await call("GET", "/ui/contacts");
    if (contacts.length === 0) {
        rows.append(node("p", "empty", "Nobody yet."));
    }
    for (const contact of contacts) {
        const row = node("button", "row");
        row.type = "button";
        const keys = node("span", "row-keys");
        keys.append(node("span", "row-key", contact.name || "unnamed"));
        keys.append(node("span", "row-dest", contact.fingerprint));
        row.append(keys);
        const tags = node("span", "row-tags");
        if (contact.pending) {
            tags.append(node("span", "tag is-pending", "pending"));
        }
        row.append(tags);
        row.append(node("span", "row-quota", contact.pending ? "accept" : "open"));
        row.onclick = async () => {
            if (contact.pending) {
                await call("POST", "/ui/contacts", {accept: contact.fingerprint});
                await render();
                return;
            }
            peer = contact.fingerprint;
            location.hash = "#/chats";
        };
        rows.append(row);
    }
    view.append(rows);

    const add = node("article", "panel-box");
    add.style.marginTop = "1rem";
    add.append(node("p", "block-title", "Add by invite"));
    const invite = node("input", "field");
    invite.type = "text";
    invite.placeholder = "bazarish://invite?…";
    const intro = node("input", "field");
    intro.type = "text";
    intro.placeholder = "a line to introduce yourself";
    add.append(node("label", "field-label", "Invite"), invite);
    add.append(node("label", "field-label", "Message"), intro);
    const failure = node("p", "hint");
    const footer = node("p", "form-footer");
    const send = node("button", "btn", "Send request");
    send.onclick = async () => {
        try {
            await call("POST", "/ui/contacts", {invite: invite.value, text: intro.value});
            await render();
        } catch (error) {
            failure.textContent = error.message;
        }
    };
    footer.append(send);
    add.append(footer, failure);
    view.append(add);
    return view;
}

async function viewApi() {
    const settings = await call("GET", "/ui/settings");
    const view = node("section", "view");
    view.append(node("h1", "view-title", "API"));
    view.append(node("p", "hint",
        "Automation opens the API with a token in the Authorization header. A token "
        + "is shown once, when it is issued: it is not kept anywhere it could be "
        + "shown again."));

    const rows = node("div", "rows");
    if (settings.tokens.length === 0) {
        rows.append(node("p", "empty", "No tokens issued."));
    }
    for (const token of settings.tokens) {
        const row = node("div", "row");
        const keys = node("span", "row-keys");
        keys.append(node("span", "row-key", token.name));
        keys.append(node("span", "row-dest",
            "issued " + new Date(token.createdAt * 1000).toLocaleDateString()));
        row.append(keys, node("span", "row-tags"));
        const revoke = node("button", "btn btn-danger btn-copy", "Revoke");
        revoke.onclick = async () => {
            await call("DELETE", "/ui/tokens/" + encodeURIComponent(token.name));
            await render();
        };
        row.append(revoke);
        rows.append(row);
    }
    view.append(rows);

    const issue = node("article", "panel-box");
    issue.style.marginTop = "1rem";
    issue.append(node("p", "block-title", "Issue a token"));
    const name = node("input", "field");
    name.type = "text";
    name.placeholder = "what this token is for";
    issue.append(node("label", "field-label", "Name"), name);
    const shown = node("pre", "copy");
    shown.hidden = true;
    const footer = node("p", "form-footer");
    const make = node("button", "btn", "Issue");
    make.onclick = async () => {
        const token = await call("POST", "/ui/tokens", {name: name.value});
        shown.hidden = false;
        shown.textContent = token.token;
        decorateCopies(issue);
        await drawRailAndNav();
    };
    footer.append(make);
    issue.append(footer, shown);
    view.append(issue);

    const how = node("article", "panel-box");
    how.style.marginTop = "1rem";
    how.append(node("p", "block-title", "How a bot uses it"));
    how.append(node("pre", "copy",
        "curl -s http://" + location.host + "/v1/events?since=0\\&wait=60 \\\n"
        + "  -H \"Authorization: Bearer $TOKEN\""));
    view.append(how);
    return view;
}

async function viewSettings() {
    const settings = await call("GET", "/ui/settings");
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Settings"));

    const transport = node("article", "panel-box");
    transport.append(node("p", "block-title", "Transport"));
    transport.append(node("p", "hint",
        "A router in this process is one thing to install; a router that is already "
        + "running is what makes several daemons on one host cheap. Either takes "
        + "effect at the next start."));
    const which = node("select", "field");
    for (const [value, label] of [["embedded", "A router in this process"],
        ["sam", "A router already running, over SAM"]]) {
        const option = node("option", null, label);
        option.value = value;
        which.append(option);
    }
    which.value = settings.transport;
    transport.append(node("label", "field-label", "I2P"), which);

    const samBlock = node("div", "block");
    samBlock.hidden = settings.transport !== "sam";
    const samHost = node("input", "field");
    samHost.type = "text";
    samHost.value = settings.samHost;
    const samPort = node("input", "field");
    samPort.type = "number";
    samPort.value = settings.samPort;
    samBlock.append(node("label", "field-label", "SAM host"), samHost);
    samBlock.append(node("label", "field-label", "SAM port"), samPort);
    transport.append(samBlock);
    which.onchange = () => { samBlock.hidden = which.value !== "sam"; };

    const privacy = node("select", "field");
    for (const [value, label] of [["minimal", "Minimal"], ["middle", "Middle"], ["max", "Max"]]) {
        const option = node("option", null, label);
        option.value = value;
        privacy.append(option);
    }
    privacy.value = settings.privacy;
    transport.append(node("label", "field-label", "Tunnel length"), privacy);

    const saved = node("p", "hint");
    const footer = node("p", "form-footer");
    const save = node("button", "btn", "Save");
    save.onclick = async () => {
        try {
            await call("POST", "/ui/settings", {
                transport: which.value,
                samHost: samHost.value,
                samPort: Number(samPort.value),
                privacy: privacy.value,
            });
            saved.textContent = "Saved. It takes effect at the next start.";
            await drawRailAndNav();
        } catch (error) {
            saved.textContent = error.message;
        }
    };
    footer.append(save);
    transport.append(footer, saved);
    view.append(transport);

    const identity = node("article", "panel-box");
    identity.style.marginTop = "1rem";
    identity.append(node("p", "block-title", "This account"));
    const who = await call("GET", "/ui/account");
    const name = node("input", "field");
    name.type = "text";
    name.value = who.name || "";
    identity.append(node("label", "field-label", "Name"), name);
    identity.append(node("p", "hint",
        "What this account calls itself, in its invite and in this panel. A contact "
        + "keeps their own name for it, so this is not sent to anybody."));
    const renamed = node("p", "hint");
    const nameFooter = node("p", "form-footer");
    const saveName = node("button", "btn", "Save");
    saveName.onclick = async () => {
        try {
            await call("POST", "/ui/account", {action: "rename", name: name.value});
            renamed.textContent = "Saved.";
            await drawRailAndNav();
        } catch (error) {
            renamed.textContent = error.message;
        }
    };
    nameFooter.append(saveName);
    identity.append(nameFooter, renamed);

    const keys = node("div", "block");
    keys.append(node("p", "block-title", "Keys"));
    keys.append(node("p", "hint",
        "A new address throws this account's destination key away and raises "
        + "another. Every invite link already handed out stops working, the old "
        + "address stops answering at once, and the contacts are sent the new "
        + "card."));
    const keyResult = node("p", "hint");
    const keyFooter = node("p", "form-footer");
    const fresh = node("button", "btn btn-danger", "New address");
    fresh.onclick = async () => {
        if (!window.confirm("The old address stops answering. Continue?")) {
            return;
        }
        keyResult.textContent = "Raising a new address…";
        try {
            await call("POST", "/ui/account", {action: "new-address"});
            keyResult.textContent = "Raised. It is published as soon as its tunnels are up.";
            await drawRailAndNav();
        } catch (error) {
            keyResult.textContent = error.message;
        }
    };
    keyFooter.append(fresh);
    keys.append(keyFooter, keyResult);
    identity.append(keys);

    const owed = who.pendingRouting || [];
    if (owed.length > 0 || who.pendingRegistry) {
        const stuck = node("div", "block");
        stuck.append(node("p", "block-title", "The new card has not reached everybody"));
        if (who.pendingRegistry) {
            stuck.append(node("p", "hint",
                "The alias registry has not taken the new address. It is asked again "
                + "every ten seconds until it does."));
        }
        for (const peer of owed) {
            const row = node("p", "form-footer");
            row.append(node("span", "hint", peer));
            const said = node("p", "hint");
            const again = node("button", "btn btn-quiet", "Retry");
            again.onclick = async () => {
                said.textContent = "Sending…";
                try {
                    await call("POST", "/ui/account", {action: "retry-routing", peer: peer});
                    said.textContent = "On its way. This list clears when it lands.";
                } catch (error) {
                    said.textContent = error.message;
                }
            };
            row.append(again);
            stuck.append(row, said);
        }
        identity.append(stuck);
    }

    const move = node("div", "block");
    move.append(node("p", "block-title", "Back this account up"));
    move.append(node("p", "hint",
        "The same .bazarish backup the desktop client writes, and the same file "
        + "restores it in either. It moves an account; it does not share one - "
        + "only one daemon may serve an account, because the daemon is that "
        + "account's server."));
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";
    move.append(node("label", "field-label", "Password for the backup"), bundlePassword);
    const wrote = node("p", "hint");
    const exportFooter = node("p", "form-footer");
    const run = node("button", "btn", "Export");
    run.onclick = async () => {
        wrote.textContent = "";
        try {
            const answer = await fetch("/ui/account/export", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({bundlePassphrase: bundlePassword.value}),
            });
            if (!answer.ok) {
                throw new Error((await answer.json()).error || answer.statusText);
            }
            const link = document.createElement("a");
            link.href = URL.createObjectURL(await answer.blob());
            const named = /filename="([^"]+)"/.exec(
                answer.headers.get("Content-Disposition") || "");
            link.download = named ? named[1] : "bazarish.bazarish";
            link.click();
            URL.revokeObjectURL(link.href);
            wrote.textContent = "Downloaded.";
        } catch (error) {
            wrote.textContent = error.message;
        }
    };
    exportFooter.append(run);
    move.append(exportFooter, wrote);
    identity.append(move);
    view.append(identity);

    const password = node("article", "panel-box");
    password.style.marginTop = "1rem";
    password.append(node("p", "block-title", "Panel password"));
    password.append(node("p", "hint",
        "The current one is asked for so a page left open cannot be used to take "
        + "the panel over, and the new one twice so a typo does not lock you out."));
    const currently = node("input", "field");
    currently.type = "password";
    currently.autocomplete = "current-password";
    const next = node("input", "field");
    next.type = "password";
    next.autocomplete = "new-password";
    const again = node("input", "field");
    again.type = "password";
    again.autocomplete = "new-password";
    password.append(node("label", "field-label", "Current password"), currently);
    password.append(node("label", "field-label", "New password"), next);
    password.append(node("label", "field-label", "New password again"), again);
    const changed = node("p", "hint");
    const passwordFooter = node("p", "form-footer");
    const change = node("button", "btn", "Change");
    change.onclick = async () => {
        try {
            await call("POST", "/ui/password",
                {current: currently.value, password: next.value, repeat: again.value});
            changed.textContent = "Changed.";
            currently.value = "";
            next.value = "";
            again.value = "";
        } catch (error) {
            changed.textContent = error.message;
        }
    };
    passwordFooter.append(change);
    password.append(passwordFooter, changed);
    view.append(password);
    return view;
}

const views = {
    overview: viewOverview,
    chats: viewChats,
    contacts: viewContacts,
    api: viewApi,
    settings: viewSettings,
};

function current() {
    const name = (location.hash || "#/overview").replace("#/", "");
    return views[name] ? name : "overview";
}

async function drawRailAndNav() {
    const state = await refreshRail();
    for (const link of document.querySelectorAll(".nav a")) {
        link.classList.toggle("is-current", link.dataset.view === current());
    }
    return state;
}

async function render() {
    const state = await drawRailAndNav();
    const name = current();
    try {
        if (name !== "overview" && !state.running) {
            const waiting = node("section", "view");
            waiting.append(node("h1", "view-title", name));
            waiting.append(node("p", "empty",
                "The account is not open yet (" + state.status + "). Overview says what to do."));
            content().replaceChildren(waiting);
            return;
        }
        if (state.running) {
            const contacts = await call("GET", "/ui/contacts");
            names = {};
            for (const contact of contacts) {
                if (contact.name) {
                    names[contact.fingerprint] = contact.name;
                }
            }
        }
        content().replaceChildren(await views[name]());
        if (name === "chats") {
            await drawChats();
            await drawThread();
        }
        decorateCopies(content());
        if (state.running && !watching) {
            watching = true;
            watch();
        }
    } catch (error) {
        const failed = node("section", "view");
        failed.append(node("h1", "view-title", "Something went wrong"));
        failed.append(node("p", "hint", error.message));
        content().replaceChildren(failed);
    }
}

async function watch() {
    while (watching) {
        try {
            const answer = await call("GET", "/ui/events?since=" + lastEventId + "&wait=45");
            lastEventId = answer.lastEventId || lastEventId;
            if (answer.events.length > 0 && current() === "chats") {
                await drawChats();
                await drawThread();
            }
        } catch (error) {
            await new Promise((resume) => setTimeout(resume, 3000));
        }
    }
}

el("signOut").onclick = async () => {
    await call("POST", "/ui/logout");
    location.href = "/";
};

window.addEventListener("hashchange", render);
render();
