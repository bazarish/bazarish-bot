// Bazarish project (c) 2026
"use strict";

const el = (id) => document.getElementById(id);
const content = () => el("content");

let accounts = [];
let accountId = null;
let peer = null;
let names = {};
let lastEventId = 0;
let watched = null;
let editing = null;
let polling = 0;

const kReactions = ["\u{1F44D}", "❤️", "\u{1F602}", "\u{1F389}", "\u{1F440}", "✅"];
const kPollMs = 1000;
const kEventWaitSeconds = 45;
const kRetryMs = 3000;
const kKinds = [
    ["bot", "Bot"],
    ["group", "Group"],
];
const kAccountViews = [
    ["overview", "Overview"],
    ["chats", "Chats"],
    ["contacts", "Contacts"],
    ["api", "API"],
    ["account", "Account"],
];

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

function base() {
    return "/ui/accounts/" + accountId;
}

function go(account, name) {
    location.hash = account ? "#/" + account + "/" + name : "#/" + name;
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

function labelOf(account) {
    return account.name || account.fingerprint.slice(0, 16) || account.id;
}

function kindPicker(value) {
    const picker = node("select", "field");
    for (const [kind, label] of kKinds) {
        const option = node("option", null, label);
        option.value = kind;
        picker.append(option);
    }
    picker.value = value || "bot";
    return picker;
}

function kindHint() {
    return node("p", "hint",
        "A bot sends to every contact from an address of its own, like any client. "
        + "A group sends to all its members from one address: a post reaches them "
        + "faster, and a server holding several members can tell they share a group.");
}

function progressText(done, total) {
    return total > 0 ? Math.round(done * 100 / total) + "% of " + total + " bytes" : "";
}

function poll(step) {
    const mine = ++polling;
    const tick = async () => {
        if (mine !== polling) {
            return;
        }
        if (await step()) {
            setTimeout(tick, kPollMs);
        }
    };
    tick();
}

async function drawSidebar() {
    accounts = await call("GET", "/ui/accounts");
    const list = el("accounts");
    list.textContent = "";
    for (const account of accounts) {
        const item = node("li");
        const link = node("a", account.id === accountId ? "is-current" : "");
        link.href = "#/" + account.id + "/overview";
        link.append(node("span", "dot " + (account.running ? "is-up" : "is-down")));
        link.append(node("span", "account-name", labelOf(account)));
        link.append(node("span", "tag" + (account.kind === "group" ? " is-live" : ""),
            account.kind));
        item.append(link);
        list.append(item);
    }

    const nav = el("nav");
    nav.textContent = "";
    const route = currentRoute();
    if (accountId) {
        for (const [name, label] of kAccountViews) {
            const item = node("li");
            const link = node("a", route.view === name ? "is-current" : "", label);
            link.href = "#/" + accountId + "/" + name;
            item.append(link);
            nav.append(item);
        }
    }
    for (const [name, label] of [["add", "Add account"], ["settings", "Settings"]]) {
        const item = node("li");
        const link = node("a", route.view === name ? "is-current" : "", label);
        link.href = "#/" + name;
        item.append(link);
        nav.append(item);
    }
}

function currentRoute() {
    const parts = (location.hash || "").replace(/^#\/?/, "").split("/");
    if (parts[0] === "add" || parts[0] === "settings") {
        return {account: null, view: parts[0]};
    }
    const known = accounts.some((account) => account.id === parts[0]);
    if (known) {
        const view = kAccountViews.some(([name]) => name === parts[1]) ? parts[1] : "overview";
        return {account: parts[0], view};
    }
    if (accounts.length > 0) {
        return {account: accounts[0].id, view: "overview"};
    }
    return {account: null, view: "add"};
}

async function viewOverview() {
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", labelOf(account)));

    if (!account.running) {
        view.append(unlockCard(account));
        return view;
    }

    const cards = node("div", "cards is-stacked");
    const identity = node("article", "card");
    const head = node("div", "card-head");
    head.append(node("span", "dot is-up"));
    head.append(node("span", "card-name", account.name || "unnamed account"));
    head.append(node("span", "tag" + (account.kind === "group" ? " is-live" : ""), account.kind));
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
    invite.append(node("pre", "copy", account.invite || ""));
    cards.append(invite);

    view.append(cards);
    return view;
}

function unlockCard(account) {
    const card = node("article", "panel-box");
    if (account.status === "starting") {
        card.append(node("p", "view-title", "Starting"));
        card.append(node("p", "step-note",
            "The account is coming up: its server side, its address and its tunnels."));
        poll(async () => {
            const now = await call("GET", base());
            if (now.status === "starting") {
                return true;
            }
            await render();
            return false;
        });
        return card;
    }
    card.append(node("p", "view-title", "The account is not running"));
    card.append(node("p", "step-note", "Status: " + account.status));
    const footer = node("p", "form-footer");
    const open = node("button", "btn", "Start");
    open.onclick = async () => {
        try {
            await call("POST", base(), {action: "open"});
            await render();
        } catch (error) {
            footer.append(node("span", "hint", error.message));
        }
    };
    footer.append(open);
    card.append(footer);
    return card;
}

async function viewAdd() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Add account"));
    view.append(node("p", "step-note",
        "Every account here shares this host's I2P router and is its own server: "
        + "it federates on its own address and keeps its own mailbox."));

    const common = node("article", "panel-box");
    common.append(node("p", "block-title", "What it is"));
    const kind = kindPicker("bot");
    common.append(node("label", "field-label", "Kind"), kind, kindHint());
    view.append(common);

    const opened = async (id) => {
        await drawSidebar();
        go(id, "overview");
    };

    const create = node("article", "panel-box");
    create.style.marginTop = "1rem";
    create.append(node("p", "block-title", "Create"));
    const name = node("input", "field");
    name.type = "text";
    name.placeholder = "what this account calls itself";
    create.append(node("label", "field-label", "Name"), name);
    const created = node("p", "hint");
    const createFooter = node("p", "form-footer");
    const make = node("button", "btn", "Create");
    make.onclick = async () => {
        try {
            const answer = await call("POST", "/ui/accounts", {action: "create",
                name: name.value, kind: kind.value});
            await opened(answer.id);
        } catch (error) {
            created.textContent = error.message;
        }
    };
    createFooter.append(make);
    create.append(createFooter, created);
    view.append(create);

    const restore = node("article", "panel-box");
    restore.style.marginTop = "1rem";
    restore.append(node("p", "block-title", "Restore a backup"));
    restore.append(node("p", "hint",
        "A .bazarish backup from the desktop client or from this panel."));
    const bundle = node("input", "field");
    bundle.type = "file";
    bundle.accept = ".bazarish";
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";
    restore.append(node("label", "field-label", "Backup file (.bazarish)"), bundle);
    restore.append(node("label", "field-label", "The backup's password"), bundlePassword);
    const restored = node("p", "hint");
    const restoreFooter = node("p", "form-footer");
    const doRestore = node("button", "btn", "Import");
    doRestore.onclick = async () => {
        const file = bundle.files && bundle.files[0];
        if (!file) {
            restored.textContent = "Pick the backup file first.";
            return;
        }
        try {
            const answer = await call("POST", "/ui/accounts", {action: "import",
                bundle: await base64Of(file), bundlePassphrase: bundlePassword.value,
                kind: kind.value});
            await opened(answer.id);
        } catch (error) {
            restored.textContent = error.message;
        }
    };
    restoreFooter.append(doRestore);
    restore.append(restoreFooter, restored);
    view.append(restore);

    view.append(receiveCard(kind, opened));
    return view;
}

function receiveCard(kind, opened) {
    const card = node("article", "panel-box");
    card.style.marginTop = "1rem";
    card.append(node("p", "block-title", "Receive from another device"));
    card.append(node("p", "hint",
        "On the other device, start pairing for the account: it shows a link and a "
        + "code. The account comes over I2P, sealed under that code."));
    const link = node("input", "field");
    link.type = "text";
    link.placeholder = "bazarish://pair?…";
    card.append(node("label", "field-label", "Pairing link"), link);
    const status = node("p", "hint");
    const codeBlock = node("div", "block");
    codeBlock.hidden = true;
    const code = node("input", "field");
    code.type = "text";
    code.inputMode = "numeric";
    code.autocomplete = "off";
    codeBlock.append(node("label", "field-label", "Code from the other device"), code);
    const codeFooter = node("p", "form-footer");
    const sendCode = node("button", "btn", "Receive");
    codeFooter.append(sendCode);
    codeBlock.append(codeFooter);

    const footer = node("p", "form-footer");
    const start = node("button", "btn", "Connect");
    const cancel = node("button", "btn btn-quiet", "Cancel");
    cancel.hidden = true;
    footer.append(start, cancel);
    card.append(footer, codeBlock, status);

    const show = (state) => {
        start.hidden = state.active;
        cancel.hidden = !state.active;
        codeBlock.hidden = !state.needsCode;
        if (state.stage === "reaching") {
            status.textContent = "Reaching the other device over I2P…";
        } else if (state.stage === "code") {
            status.textContent = state.triesLeft > 0
                ? "Wrong code. " + state.triesLeft + " tries left."
                : "Enter the code shown on the other device.";
        } else if (state.stage === "receiving") {
            status.textContent = "Receiving the account " + progressText(state.done, state.total);
        } else if (state.stage === "failed") {
            status.textContent = state.error;
        } else {
            status.textContent = "";
        }
    };
    const follow = () => poll(async () => {
        const state = await call("GET", "/ui/pairing");
        if (state.stage === "done" && state.account) {
            await opened(state.account);
            return false;
        }
        show(state);
        return state.active;
    });

    start.onclick = async () => {
        try {
            show(await call("POST", "/ui/pairing", {link: link.value, kind: kind.value}));
            follow();
        } catch (error) {
            status.textContent = error.message;
        }
    };
    sendCode.onclick = async () => {
        try {
            show(await call("POST", "/ui/pairing", {code: code.value.trim()}));
            code.value = "";
            follow();
        } catch (error) {
            status.textContent = error.message;
        }
    };
    cancel.onclick = async () => {
        await call("DELETE", "/ui/pairing");
        show({active: false, needsCode: false, stage: ""});
    };
    call("GET", "/ui/pairing").then((state) => {
        if (state.active) {
            show(state);
            follow();
        }
    }, () => {});
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
    const chats = await call("GET", base() + "/chats");
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
    const lines = await call("GET", base() + "/messages?peer=" + encodeURIComponent(peer) + "&limit=100");
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
            await call("POST", base() + "/messages/" + target.messageId, {peer, text});
        } else {
            await call("POST", base() + "/messages", {peer, text});
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
            await call("POST", base() + "/reactions",
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
            await call("DELETE", base() + "/messages/" + line.messageId
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
    const contacts = await call("GET", base() + "/contacts");
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
                await call("POST", base() + "/contacts", {accept: contact.fingerprint});
                await render();
                return;
            }
            peer = contact.fingerprint;
            go(accountId, "chats");
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
            await call("POST", base() + "/contacts", {invite: invite.value, text: intro.value});
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
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", "API"));
    view.append(node("p", "hint",
        "Automation opens the API with a token in the Authorization header. A token "
        + "opens this account and no other, and it is shown once, when it is issued: "
        + "it is not kept anywhere it could be shown again."));

    const rows = node("div", "rows");
    if (account.tokens.length === 0) {
        rows.append(node("p", "empty", "No tokens issued."));
    }
    for (const token of account.tokens) {
        const row = node("div", "row");
        const keys = node("span", "row-keys");
        keys.append(node("span", "row-key", token.name));
        keys.append(node("span", "row-dest",
            "issued " + new Date(token.createdAt * 1000).toLocaleDateString()));
        row.append(keys, node("span", "row-tags"));
        const revoke = node("button", "btn btn-danger btn-copy", "Revoke");
        revoke.onclick = async () => {
            await call("DELETE", base() + "/tokens/" + encodeURIComponent(token.name));
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
    const failure = node("p", "hint");
    const footer = node("p", "form-footer");
    const make = node("button", "btn", "Issue");
    make.onclick = async () => {
        try {
            const token = await call("POST", base() + "/tokens", {name: name.value});
            shown.hidden = false;
            shown.textContent = token.token;
            failure.textContent = "";
            decorateCopies(issue);
        } catch (error) {
            failure.textContent = error.message;
        }
    };
    footer.append(make);
    issue.append(footer, shown, failure);
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

async function viewAccount() {
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Account"));

    if (!account.running) {
        view.append(node("p", "empty",
            "The account is not open yet (" + account.status + "). Overview says what to do."));
        return view;
    }

    const identity = node("article", "panel-box");
    identity.append(node("p", "block-title", "Name"));
    const name = node("input", "field");
    name.type = "text";
    name.value = account.name || "";
    identity.append(name);
    identity.append(node("p", "hint",
        "What this account calls itself, in its invite and in this panel. A contact "
        + "keeps their own name for it, so this is not sent to anybody."));
    const renamed = node("p", "hint");
    const nameFooter = node("p", "form-footer");
    const saveName = node("button", "btn", "Save");
    saveName.onclick = async () => {
        try {
            await call("POST", base(), {action: "rename", name: name.value});
            renamed.textContent = "Saved.";
            await drawSidebar();
        } catch (error) {
            renamed.textContent = error.message;
        }
    };
    nameFooter.append(saveName);
    identity.append(nameFooter, renamed);
    view.append(identity);

    const kindBox = node("article", "panel-box");
    kindBox.style.marginTop = "1rem";
    kindBox.append(node("p", "block-title", "Kind"));
    const kind = kindPicker(account.kind);
    kindBox.append(kind, kindHint());
    const kindSaid = node("p", "hint");
    const kindFooter = node("p", "form-footer");
    const saveKind = node("button", "btn", "Save");
    saveKind.onclick = async () => {
        try {
            await call("POST", base(), {action: "kind", kind: kind.value});
            kindSaid.textContent = "Saved. The next delivery leaves the new way.";
            await drawSidebar();
        } catch (error) {
            kindSaid.textContent = error.message;
        }
    };
    kindFooter.append(saveKind);
    kindBox.append(kindFooter, kindSaid);
    view.append(kindBox);

    const keys = node("article", "panel-box");
    keys.style.marginTop = "1rem";
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
            await call("POST", base(), {action: "new-address"});
            keyResult.textContent = "Raised. It is published as soon as its tunnels are up.";
        } catch (error) {
            keyResult.textContent = error.message;
        }
    };
    keyFooter.append(fresh);
    keys.append(keyFooter, keyResult);

    const owed = account.pendingRouting || [];
    if (owed.length > 0 || account.pendingRegistry) {
        const stuck = node("div", "block");
        stuck.append(node("p", "block-title", "The new card has not reached everybody"));
        if (account.pendingRegistry) {
            stuck.append(node("p", "hint",
                "The alias registry has not taken the new address. It is asked again "
                + "every ten seconds until it does."));
        }
        for (const owedPeer of owed) {
            const row = node("p", "form-footer");
            row.append(node("span", "hint", owedPeer));
            const said = node("p", "hint");
            const again = node("button", "btn btn-quiet", "Retry");
            again.onclick = async () => {
                said.textContent = "Sending…";
                try {
                    await call("POST", base(), {action: "retry-routing", peer: owedPeer});
                    said.textContent = "On its way. This list clears when it lands.";
                } catch (error) {
                    said.textContent = error.message;
                }
            };
            row.append(again);
            stuck.append(row, said);
        }
        keys.append(stuck);
    }
    view.append(keys);

    view.append(exportCard(), pairCard(), removeCard());
    return view;
}

function exportCard() {
    const move = node("article", "panel-box");
    move.style.marginTop = "1rem";
    move.append(node("p", "block-title", "Back this account up"));
    move.append(node("p", "hint",
        "The same .bazarish backup the desktop client writes, and the same file "
        + "restores it in either. It moves an account; it does not share one: only "
        + "one place may serve an account, because this host is that account's "
        + "server."));
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";
    move.append(node("label", "field-label", "Password for the backup"), bundlePassword);
    const wrote = node("p", "hint");
    const exportFooter = node("p", "form-footer");
    const run = node("button", "btn", "Export");
    run.onclick = async () => {
        wrote.textContent = "";
        try {
            const answer = await fetch(base() + "/export", {
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
    return move;
}

function pairCard() {
    const card = node("article", "panel-box");
    card.style.marginTop = "1rem";
    card.append(node("p", "block-title", "Send to another device"));
    card.append(node("p", "hint",
        "Pairing hands the account over I2P to a desktop client or another panel: "
        + "open the link there and type the code. Like a backup, it moves the "
        + "account; it does not share it."));
    const offer = node("div");
    offer.hidden = true;
    const link = node("pre", "copy");
    const code = node("p", "pair-code");
    offer.append(link, code);
    const status = node("p", "hint");
    const footer = node("p", "form-footer");
    const start = node("button", "btn", "Start pairing");
    const stop = node("button", "btn btn-quiet", "Stop");
    stop.hidden = true;
    footer.append(start, stop);
    card.append(footer, offer, status);

    const show = (state) => {
        start.hidden = state.active;
        stop.hidden = !state.active;
        offer.hidden = !state.active;
        if (state.active) {
            link.textContent = state.uri;
            code.textContent = state.code;
            decorateCopies(card);
        }
        if (state.stage === "publishing") {
            status.textContent = "Publishing the pairing address…";
        } else if (state.stage === "waiting") {
            status.textContent = "Waiting for the other device.";
        } else if (state.stage === "wrong-code") {
            status.textContent = "A wrong code was tried " + state.wrongCodes + " times.";
        } else if (state.stage === "sending") {
            status.textContent = "Sending the account " + progressText(state.done, state.total);
        } else if (state.stage === "done") {
            status.textContent = "The account is on the other device.";
        } else if (state.stage === "refused") {
            status.textContent = "Too many wrong codes: pairing stopped.";
        } else if (state.stage === "failed") {
            status.textContent = state.error;
        } else {
            status.textContent = "";
        }
    };
    const follow = () => poll(async () => {
        const state = await call("GET", base() + "/pairing");
        show(state);
        return state.active;
    });

    start.onclick = async () => {
        try {
            show(await call("POST", base() + "/pairing"));
            follow();
        } catch (error) {
            status.textContent = error.message;
        }
    };
    stop.onclick = async () => {
        await call("DELETE", base() + "/pairing");
        show({active: false, stage: ""});
    };
    call("GET", base() + "/pairing").then((state) => {
        if (state.active) {
            show(state);
            follow();
        }
    }, () => {});
    return card;
}

function removeCard() {
    const card = node("article", "panel-box");
    card.style.marginTop = "1rem";
    card.append(node("p", "block-title", "Remove this account"));
    card.append(node("p", "hint",
        "Closes the account on its server, which is this host, and deletes its file, "
        + "its mailbox, its conversations and its tokens. Export it first if it is to "
        + "live on somewhere else."));
    const said = node("p", "hint");
    const footer = node("p", "form-footer");
    const drop = node("button", "btn btn-danger", "Remove");
    drop.onclick = async () => {
        if (!window.confirm("The account and everything it holds here is deleted. Continue?")) {
            return;
        }
        try {
            await call("DELETE", base());
            accountId = null;
            await drawSidebar();
            go(null, "add");
        } catch (error) {
            said.textContent = error.message;
        }
    };
    footer.append(drop);
    card.append(footer, said);
    return card;
}

async function viewSettings() {
    const settings = await call("GET", "/ui/settings");
    const view = node("section", "view");
    view.append(node("h1", "view-title", "Settings"));

    const transport = node("article", "panel-box");
    transport.append(node("p", "block-title", "Transport"));
    transport.append(node("p", "hint",
        "One router carries every account here. A router in this process is one thing "
        + "to install; a router that is already running can be shared with other "
        + "programs. Either takes effect at the next start."));
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
        } catch (error) {
            saved.textContent = error.message;
        }
    };
    footer.append(save);
    transport.append(footer, saved);
    view.append(transport);

    const operator = node("article", "panel-box");
    operator.style.marginTop = "1rem";
    operator.append(node("p", "block-title", "Operator"));
    operator.append(node("p", "hint",
        "The key that claimed this host signs in here, and no other. It is the "
        + "\"operator\" in the config file; to hand the host over, change it there."));
    operator.append(facts([["Fingerprint", settings.operator]]));
    view.append(operator);
    return view;
}

const views = {
    overview: viewOverview,
    chats: viewChats,
    contacts: viewContacts,
    api: viewApi,
    account: viewAccount,
    add: viewAdd,
    settings: viewSettings,
};

async function render() {
    polling++;
    try {
        accounts = await call("GET", "/ui/accounts");
        const route = currentRoute();
        if (route.account !== accountId) {
            accountId = route.account;
            peer = null;
            editing = null;
            names = {};
            lastEventId = 0;
        }
        await drawSidebar();
        const summary = accounts.find((account) => account.id === accountId);
        const needsOpen = ["chats", "contacts", "api"].includes(route.view);
        if (needsOpen && summary && !summary.running) {
            const waiting = node("section", "view");
            waiting.append(node("h1", "view-title", route.view));
            waiting.append(node("p", "empty",
                "The account is not open yet (" + summary.status + "). Overview says what to do."));
            content().replaceChildren(waiting);
            return;
        }
        if (summary && summary.running) {
            const contacts = await call("GET", base() + "/contacts");
            names = {};
            for (const contact of contacts) {
                if (contact.name) {
                    names[contact.fingerprint] = contact.name;
                }
            }
        }
        content().replaceChildren(await views[route.view]());
        if (route.view === "chats") {
            await drawChats();
            await drawThread();
        }
        decorateCopies(content());
        if (summary && summary.running && watched !== accountId) {
            watch(accountId);
        }
    } catch (error) {
        const failed = node("section", "view");
        failed.append(node("h1", "view-title", "Something went wrong"));
        failed.append(node("p", "hint", error.message));
        content().replaceChildren(failed);
    }
}

async function watch(account) {
    watched = account;
    while (watched === account) {
        try {
            const answer = await call("GET", "/ui/accounts/" + account + "/events?since="
                + lastEventId + "&wait=" + kEventWaitSeconds);
            if (watched !== account) {
                return;
            }
            lastEventId = answer.lastEventId || lastEventId;
            if (answer.events.length > 0 && currentRoute().view === "chats") {
                await drawChats();
                await drawThread();
            }
        } catch (error) {
            await new Promise((resume) => setTimeout(resume, kRetryMs));
        }
    }
}

el("signOut").onclick = async () => {
    await call("POST", "/ui/logout");
    location.href = "/";
};

window.addEventListener("hashchange", render);
render();
