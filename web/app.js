// Bazarish project (c) 2026
"use strict";

const el = (id) => document.getElementById(id);
const content = () => el("content");

let accounts = [];
let accountId = null;
let peer = null;
let menuOpen = true;
let names = {};
let lastEventId = 0;
let watched = null;
let editing = null;
let polling = 0;

const kReactions = ["\u{1F44D}", "❤️", "\u{1F602}", "\u{1F389}", "\u{1F440}", "✅"];
const kPollMs = 1000;
const kEventWaitSeconds = 45;
const kRetryMs = 3000;
const kAliasSigil = "!";
const kKinds = [
    ["bot", N_("Bot")],
    ["group", N_("Group")],
];
const kAccountViews = [
    ["overview", N_("Overview")],
    ["chats", N_("Chats")],
    ["contacts", N_("Contacts")],
    ["aliases", N_("Aliases")],
    ["sign", N_("Sign in")],
    ["api", N_("API")],
    ["account", N_("Account")],
];
const kGroupViews = [
    ["overview", N_("Overview")],
    ["group", N_("Group")],
    ["members", N_("Members")],
    ["requests", N_("Requests")],
    ["posts", N_("Posts")],
    ["aliases", N_("Aliases")],
    ["sign", N_("Sign in")],
    ["account", N_("Account")],
];
const kMemberFilters = [
    ["all", N_("Everyone")],
    ["member", N_("In the group")],
    ["admin", N_("Administrators")],
    ["pending", N_("Waiting")],
    ["left", N_("Left")],
    ["banned", N_("Blocked")],
];
const kStateLabels = {
    naming: N_("choosing a name"),
    pending: N_("waiting"),
    member: N_("member"),
    gone: N_("unreachable"),
    left: N_("left"),
    banned: N_("blocked"),
};
const kPostsPage = 100;
let memberFilter = "all";
let memberQuery = "";

async function call(method, path, body) {
    const options = {method, headers: {}};
    if (body !== undefined) {
        options.headers["Content-Type"] = "application/json";
        options.body = JSON.stringify(body);
    }
    const answer = await fetch(path, options);
    if (answer.status === 401) {
        location.href = "/";
        throw new Error(t("signed out"));
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
    for (const block of root.querySelectorAll("pre.copy:not([data-copyable]):not([hidden])")) {
        block.dataset.copyable = "yes";
        block.parentNode.insertBefore(
            marks.button(t("Copy"), () => block.textContent), block.nextSibling);
    }
    for (const value of root.querySelectorAll("[data-copy]:not([data-copyable])")) {
        value.dataset.copyable = "yes";
        value.append(marks.icon(() => value.dataset.copy, t("Copy")));
    }
}

function facts(pairs, copyable = true) {
    const list = node("ul", "facts");
    for (const [label, value] of pairs) {
        const row = node("li");
        row.append(node("span", "fact-label", label));
        const held = node("b");
        const inner = node("span", "fact-value", value);
        if (copyable) {
            inner.dataset.copy = value;
        }
        held.append(inner);
        row.append(held);
        list.append(row);
    }
    return list;
}

function base64Of(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onerror = () => reject(new Error(t("that file could not be read")));
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
        const option = node("option", null, t(label));
        option.value = kind;
        picker.append(option);
    }
    picker.value = value || "bot";
    return picker;
}

function kindHint() {
    return node("p", "hint",
        t("A bot writes to each contact from a separate address. A group writes to all members from one address. Its posts arrive faster, and a server holding several members sees that they share a group."));
}

function progressText(done, total) {
    return total > 0 ? t("%1% of %2 bytes", Math.round(done * 100 / total), total) : "";
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
        const current = account.id === accountId;
        const link = node("a", current ? "is-current" + (menuOpen ? " is-open" : "") : "");
        link.href = "#/" + account.id + "/overview";
        link.append(node("span", "dot " + (account.running ? "is-up" : "is-down")));
        link.append(node("span", "account-name", labelOf(account)));
        link.append(node("span", "tag" + (account.kind === "group" ? " is-key" : ""),
            t(account.kind === "group" ? "Group" : "Bot")));
        item.append(link);
        if (current) {
            link.setAttribute("aria-expanded", String(menuOpen));
            link.onclick = (event) => {
                event.preventDefault();
                menuOpen = !menuOpen;
                drawSidebar();
            };
            if (menuOpen) {
                item.append(accountMenu(account));
            }
        }
        list.append(item);
    }

    const nav = el("nav");
    nav.textContent = "";
    const route = currentRoute();
    for (const [name, label] of [["add", N_("Add account")], ["settings", N_("Settings")],
        ["system", N_("System")]]) {
        const item = node("li");
        const link = node("a", route.view === name ? "is-current" : "", t(label));
        link.href = "#/" + name;
        item.append(link);
        nav.append(item);
    }
}

function accountMenu(account) {
    const route = currentRoute();
    const menu = node("ul", "account-nav");
    for (const [name, label] of viewsFor(account)) {
        const item = node("li");
        const link = node("a", route.view === name ? "is-current" : "", t(label));
        link.href = "#/" + account.id + "/" + name;
        item.append(link);
        menu.append(item);
    }
    return menu;
}

function viewsFor(account) {
    return account && account.kind === "group" ? kGroupViews : kAccountViews;
}

function viewLabel(name) {
    const known = kAccountViews.concat(kGroupViews).find(([view]) => view === name);
    return known ? t(known[1]) : name;
}

function currentRoute() {
    const parts = (location.hash || "").replace(/^#\/?/, "").split("/");
    if (["add", "settings", "system"].includes(parts[0])) {
        return {account: null, view: parts[0]};
    }
    const known = accounts.find((account) => account.id === parts[0]);
    if (known) {
        const view = viewsFor(known).some(([name]) => name === parts[1]) ? parts[1] : "overview";
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
    head.append(node("span", "card-name", account.name || t("unnamed account")));
    head.append(node("span", "tag" + (account.kind === "group" ? " is-key" : ""),
        t(account.kind === "group" ? "Group" : "Bot")));
    identity.append(head);
    identity.append(facts([
        [t("Fingerprint"), account.fingerprint],
        [t("Address"), account.destination || t("building…")],
    ]));
    cards.append(identity);

    const invite = node("article", "card");
    const inviteHead = node("div", "card-head");
    inviteHead.append(node("span", "card-name", t("Invite")));
    invite.append(inviteHead);
    invite.append(node("p", "hint", account.kind === "group"
        ? t("Members join with this invite.")
        : t("Anyone with the invite can write to this account.")));
    invite.append(node("pre", "copy", account.invite || ""));
    cards.append(invite);

    view.append(cards);
    return view;
}

function unlockCard(account) {
    const card = node("article", "panel-box");
    if (account.status === "starting") {
        card.append(node("p", "view-title", t("Starting")));
        card.append(node("p", "step-note",
            t("The account is starting.")));
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
    card.append(node("p", "view-title", t("The account is not running")));
    card.append(node("p", "step-note", t("Status: %1", account.status)));
    const footer = node("p", "form-footer");
    const open = node("button", "btn", t("Start"));
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
    view.append(node("h1", "view-title", t("Add account")));
    view.append(node("p", "step-note",
        t("Each account is a separate server with its own address and mailbox.")));

    const common = node("article", "panel-box");
    const kind = kindPicker("bot");
    common.append(node("label", "field-label", t("Kind")), kind, kindHint());
    view.append(common);

    const opened = async (id) => {
        await drawSidebar();
        go(id, "overview");
    };

    const create = node("article", "panel-box");
    create.append(node("p", "block-title", t("Create")));
    const name = node("input", "field");
    name.type = "text";
    name.placeholder = t("account name");
    create.append(node("label", "field-label", t("Name")), name);
    const created = node("p", "hint");
    const createFooter = node("p", "form-footer");
    const make = node("button", "btn", t("Create"));
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

    const restore = node("article", "panel-box");
    restore.append(node("p", "block-title", t("Restore a backup")));
    restore.append(node("p", "hint",
        t("A backup from the desktop client or from this panel.")));
    const bundle = node("input", "field");
    bundle.type = "file";
    bundle.accept = ".bazarish";
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";
    restore.append(node("label", "field-label", t("Backup file (.bazarish)")), bundle);
    restore.append(node("label", "field-label", t("The backup's password")), bundlePassword);
    const restored = node("p", "hint");
    const restoreFooter = node("p", "form-footer");
    const doRestore = node("button", "btn", t("Import"));
    doRestore.onclick = async () => {
        const file = bundle.files && bundle.files[0];
        if (!file) {
            restored.textContent = t("Pick the backup file first.");
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

    view.append(tabbed([create, restore, receiveCard(kind, opened)]));
    return view;
}

function tabbed(cards) {
    const box = node("article", "panel-box tabbed");
    const bar = node("div", "tab-bar");
    bar.setAttribute("role", "tablist");
    const tabs = [];
    const panels = [];
    const choose = (chosen) => {
        tabs.forEach((tab, index) => {
            const on = index === chosen;
            tab.classList.toggle("is-on", on);
            tab.setAttribute("aria-selected", String(on));
            tab.tabIndex = on ? 0 : -1;
            panels[index].hidden = !on;
        });
    };
    cards.forEach((card, index) => {
        const title = card.querySelector(".block-title");
        const tab = node("button", "tab", title.textContent);
        tab.type = "button";
        tab.setAttribute("role", "tab");
        tab.onclick = () => choose(index);
        tab.onkeydown = (event) => {
            const step = {ArrowRight: 1, ArrowLeft: -1}[event.key];
            if (step === undefined) {
                return;
            }
            const rtl = document.documentElement.dir === "rtl";
            const next = (index + (rtl ? -step : step) + cards.length) % cards.length;
            choose(next);
            tabs[next].focus();
        };
        title.remove();
        const panel = node("div", "tab-panel");
        panel.setAttribute("role", "tabpanel");
        panel.append(...card.childNodes);
        tabs.push(tab);
        panels.push(panel);
        bar.append(tab);
    });
    box.append(bar, ...panels);
    choose(0);
    return box;
}

function receiveCard(kind, opened) {
    const card = node("article", "panel-box");
    card.append(node("p", "block-title", t("Receive from another device")));
    card.append(node("p", "hint",
        t("Start sending the account on the other device. It shows a link and a code.")));
    const link = node("input", "field");
    link.type = "text";
    link.placeholder = "bazarish://pair?…";
    card.append(node("label", "field-label", t("Transfer link")), link);
    const status = node("p", "hint");
    const codeBlock = node("div", "block");
    codeBlock.hidden = true;
    const code = node("input", "field");
    code.type = "text";
    code.inputMode = "numeric";
    code.autocomplete = "off";
    codeBlock.append(node("label", "field-label", t("Code from the other device")), code);
    const codeFooter = node("p", "form-footer");
    const sendCode = node("button", "btn", t("Receive"));
    codeFooter.append(sendCode);
    codeBlock.append(codeFooter);

    const footer = node("p", "form-footer");
    const start = node("button", "btn", t("Connect"));
    const cancel = node("button", "btn btn-quiet", t("Cancel"));
    cancel.hidden = true;
    footer.append(start, cancel);
    card.append(footer, codeBlock, status);

    const show = (state) => {
        start.hidden = state.active;
        cancel.hidden = !state.active;
        codeBlock.hidden = !state.needsCode;
        if (state.stage === "reaching") {
            status.textContent = t("Connecting to the other device…");
        } else if (state.stage === "code") {
            status.textContent = state.triesLeft > 0
                ? t("Wrong code. %1 tries left.", state.triesLeft)
                : t("Enter the code shown on the other device.");
        } else if (state.stage === "receiving") {
            status.textContent = t("Receiving the account %1", progressText(state.done, state.total));
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
    const view = node("section", "view is-chat");
    view.append(node("h1", "view-title", t("Chats")));

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
        list.append(node("p", "empty", t("No conversations yet.")));
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
        return line.outgoing ? t("you: %1", line.text) : line.text;
    }
    return "[" + line.type + "]";
}

async function drawThread() {
    const thread = el("thread");
    if (!thread) {
        return;
    }
    const typed = thread.querySelector(".composer .field");
    const draft = typed && typed.dataset.peer === peer ? typed.value : "";
    thread.textContent = "";
    if (!peer) {
        thread.append(node("p", "empty", t("Pick a conversation.")));
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
    field.placeholder = editing ? t("Edit the message") : t("Write a message");
    field.autocomplete = "off";
    field.value = editing ? editing.text : draft;
    field.dataset.peer = peer;
    const send = node("button", "btn", editing ? t("Save") : t("Send"));
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

const kDeliveryMarks = {
    sending: ["is-sending", "◷", N_("Sending")],
    delivered: ["is-delivered", "✓", N_("Accepted by their server")],
    read: ["is-read", "✓✓", N_("Read")],
};

function deliveryMark(status) {
    const known = kDeliveryMarks[status];
    if (known) {
        const mark = node("span", "delivery " + known[0], known[1]);
        mark.title = t(known[2]);
        mark.setAttribute("aria-label", t(known[2]));
        return mark;
    }
    const failed = node("span", "delivery is-failed", "!");
    const reason = status.replace(/^failed:?\s*/, "");
    failed.title = reason ? t("Not delivered: %1", reason) : t("Not delivered");
    failed.setAttribute("aria-label", failed.title);
    return failed;
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
            chip.title = reaction.reactor === "me" ? t("you") : nameOf(reaction.reactor);
            reactions.append(chip);
        }
        held.append(reactions);
    }

    const meta = node("div", "line-meta");
    meta.textContent = when(line.sentAt);
    if (line.edited) {
        meta.append(node("span", "edited", t(" · edited")));
    }
    if (line.outgoing && line.status) {
        meta.append(deliveryMark(line.status));
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
        button.title = mine ? t("take it back") : t("react");
        button.onclick = async () => {
            await call("POST", base() + "/reactions",
                {peer, messageId: line.messageId, emoji: mine ? "" : emoji});
            await drawThread();
        };
        tools.append(button);
    }
    if (line.outgoing && line.type === "text") {
        const edit = node("button", "line-tool", t("edit"));
        edit.type = "button";
        edit.onclick = () => { editing = line; drawThread(); };
        const drop = node("button", "line-tool", t("delete"));
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
    view.append(node("h1", "view-title", t("Contacts")));

    const account = await call("GET", base());
    const automatic = node("label", "check check-lead");
    const accepts = node("input");
    accepts.type = "checkbox";
    accepts.checked = account.acceptsContacts;
    automatic.append(accepts, node("span", null, t("Accept contact requests automatically")));
    accepts.onchange = async () => {
        failure.textContent = "";
        try {
            await call("POST", base(), {action: "accept-contacts", accepts: accepts.checked});
        } catch (error) {
            accepts.checked = !accepts.checked;
            failure.textContent = error.message;
        }
    };
    view.append(automatic);

    const rows = node("div", "rows");
    const contacts = await call("GET", base() + "/contacts");
    if (contacts.length === 0) {
        rows.append(node("p", "empty", t("Nobody yet.")));
    }
    for (const contact of contacts) {
        const row = node("button", "row");
        row.type = "button";
        const keys = node("span", "row-keys");
        keys.append(node("span", "row-key", contact.name || t("no name yet")));
        keys.append(node("span", "row-dest", contact.fingerprint));
        row.append(keys);
        const tags = node("span", "row-tags");
        if (contact.pending) {
            tags.append(node("span", "tag is-pending",
                contact.accepting ? t("accepting…") : t("pending")));
        }
        row.append(tags);
        const action = node("span", "row-quota", contact.pending
            ? (contact.accepting ? "" : t("Accept")) : t("Open the chat"));
        row.append(action);
        row.disabled = contact.accepting;
        row.onclick = async () => {
            if (!contact.pending) {
                peer = contact.fingerprint;
                go(accountId, "chats");
                return;
            }
            row.disabled = true;
            failure.textContent = "";
            try {
                await call("POST", base() + "/contacts", {accept: contact.fingerprint});
                await render();
            } catch (error) {
                row.disabled = false;
                failure.textContent = error.message;
            }
        };
        rows.append(row);
    }
    const failure = node("p", "hint");
    view.append(rows, failure);

    const add = node("article", "panel-box");
    add.append(node("p", "block-title", t("Add a contact")));
    const invite = node("input", "field");
    invite.type = "text";
    invite.placeholder = t("bazarish://invite?… or %1alias", kAliasSigil);
    const intro = node("input", "field");
    intro.type = "text";
    intro.placeholder = t("a line to introduce yourself");
    add.append(node("label", "field-label", t("Invite or alias")), invite);
    add.append(node("label", "field-label", t("Message")), intro);
    const sendFailure = node("p", "hint");
    const footer = node("p", "form-footer");
    const send = node("button", "btn", t("Send request"));
    send.onclick = async () => {
        try {
            const typed = invite.value.trim();
            await call("POST", base() + "/contacts", typed.startsWith(kAliasSigil)
                ? {alias: typed, text: intro.value}
                : {invite: typed, text: intro.value});
            await render();
        } catch (error) {
            sendFailure.textContent = error.message;
        }
    };
    footer.append(send);
    add.append(footer, sendFailure);
    view.append(add);
    return view;
}

function aliasTerm(alias) {
    const day = new Date(alias.notAfter * 1000).toLocaleDateString();
    return alias.autoRenew ? t("renews %1", day) : t("expires %1", day);
}

function aliasState(alias) {
    if (!alias.resolving) {
        return t("answers nobody");
    }
    return alias.live ? aliasTerm(alias) : t("pointing it here…");
}

async function viewAliases() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Aliases")));
    view.append(node("p", "hint",
        t("Aliases are bought in the alias registry. Sign in there with a signature from the Sign in page.")));

    const rows = node("div", "rows");
    const note = node("p", "hint");
    const outcome = node("p", "hint");
    const check = node("button", "btn", t("Check aliases"));

    const draw = (state) => {
        rows.textContent = "";
        if (state.aliases.length === 0) {
            rows.append(node("p", "empty", t("No alias is registered to this account.")));
        }
        for (const alias of state.aliases) {
            const row = node("div", "row");
            const keys = node("span", "row-keys");
            const name = node("span", "row-key", kAliasSigil + alias.alias);
            name.dataset.copy = kAliasSigil + alias.alias;
            keys.append(name, node("span", "row-dest", aliasState(alias)));
            const tags = node("span", "row-tags");
            if (alias.live) {
                tags.append(node("span", "tag is-live", t("points here")));
            }
            row.append(keys, tags);
            const toggle = node("button", "btn btn-copy",
                alias.resolving ? t("Point nowhere") : t("Point here"));
            toggle.onclick = async () => {
                toggle.disabled = true;
                try {
                    draw(await call("POST", base() + "/aliases",
                        {alias: alias.alias, resolving: !alias.resolving}));
                } catch (error) {
                    outcome.textContent = error.message;
                    toggle.disabled = false;
                }
            };
            row.append(toggle);
            rows.append(row);
        }
        decorateCopies(rows);
        note.textContent = state.aliases.length > 0 && !state.depositCovers
            ? t("The deposit will not cover the next renewal.") : "";
        check.disabled = state.checking;
        check.textContent = state.checking ? t("Asking…") : t("Check aliases");
        if (state.checked) {
            outcome.textContent = state.checked.ok && state.aliases.length === 0
                ? "" : state.checked.note;
        }
    };

    const follow = () => poll(async () => {
        const state = await call("GET", base() + "/aliases");
        draw(state);
        return state.checking;
    });

    check.onclick = async () => {
        outcome.textContent = "";
        try {
            draw(await call("POST", base() + "/aliases", {check: true}));
            follow();
        } catch (error) {
            outcome.textContent = error.message;
        }
    };

    const state = await call("GET", base() + "/aliases");
    draw(state);
    if (state.checking) {
        follow();
    }
    view.append(rows, note);
    const footer = node("p", "form-footer");
    footer.append(check);
    view.append(footer, outcome);
    return view;
}

async function viewSign() {
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Sign in")));
    view.append(node("p", "hint",
        t("Paste the challenge from a site that signs in with a Bazarish key. The signature lets you in there as %1.",
            labelOf(account))));

    const box = node("article", "panel-box");
    const challenge = node("textarea", "field");
    challenge.rows = 5;
    challenge.placeholder = t("Paste the challenge");
    box.append(node("label", "field-label", t("Challenge from the site")), challenge);

    const consumer = node("div");
    const signature = node("pre", "copy");
    const signed = node("div");
    signed.hidden = true;
    signed.append(node("label", "field-label", t("Signature")), signature);
    const failure = node("p", "hint");
    const footer = node("p", "form-footer");
    const sign = node("button", "btn", t("Sign"));
    sign.disabled = true;

    const describe = async () => {
        signed.hidden = true;
        failure.textContent = "";
        consumer.textContent = "";
        const typed = challenge.value.trim();
        sign.disabled = true;
        if (!typed) {
            return;
        }
        const seen = await call("POST", base() + "/sign", {challenge: typed});
        if (challenge.value.trim() !== typed) {
            return;
        }
        if (!seen.ok) {
            consumer.append(node("p", "hint", t("This challenge cannot be signed: %1", seen.problem)));
            return;
        }
        consumer.append(node("p", "field-label", t("You are signing in to")));
        consumer.append(node("p", "block-title", seen.name));
        for (const place of seen.place) {
            consumer.append(node("p", "row-dest", place));
        }
        if (seen.role) {
            consumer.append(node("p", "hint", t("as %1", seen.role)));
        }
        consumer.append(node("p", "hint", seen.place.length > 1
            ? t("Do not sign if the site you came from is not in the list.")
            : t("Do not sign if this is not the site you came from.")));
        sign.disabled = false;
    };
    challenge.oninput = () => describe().catch((error) => {
        failure.textContent = error.message;
    });

    sign.onclick = async () => {
        failure.textContent = "";
        try {
            const answer = await call("POST", base() + "/sign",
                {challenge: challenge.value.trim(), sign: true});
            signature.textContent = answer.signature;
            signed.hidden = false;
            decorateCopies(box);
        } catch (error) {
            failure.textContent = error.message;
        }
    };
    footer.append(sign);
    box.append(consumer, footer, signed, failure);
    view.append(box);
    return view;
}

const kBotDocs = "https://bazari.sh/docs/bots";

function linkedText(text, href) {
    const holder = node("span");
    const match = /^(.*)\[(.*)\](.*)$/.exec(text);
    if (!match) {
        holder.textContent = text;
        return holder;
    }
    const link = node("a", null, match[2]);
    link.href = href;
    link.target = "_blank";
    link.rel = "noopener";
    holder.append(match[1], link, match[3]);
    return holder;
}

function tokenRow(token) {
    const row = node("div", "row");
    const keys = node("span", "row-keys");
    keys.append(node("span", "row-key", token.name));
    keys.append(node("span", "row-dest",
        t("issued %1", new Date(token.createdAt * 1000).toLocaleDateString())));
    row.append(keys, node("span", "row-tags"));
    const revoke = node("button", "btn btn-danger btn-copy", t("Revoke"));
    revoke.onclick = async () => {
        await call("DELETE", base() + "/tokens/" + encodeURIComponent(token.name));
        await render();
    };
    row.append(revoke);
    return row;
}

async function viewApi() {
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("API")));
    view.append(node("p", "hint",
        t("The API takes a token in the Authorization header. A token opens this account only. It is shown once, when issued.")));

    const rows = node("div", "rows");
    const none = node("p", "empty", t("No tokens issued."));
    none.hidden = account.tokens.length > 0;
    rows.append(none);
    for (const token of account.tokens) {
        rows.append(tokenRow(token));
    }

    const issue = node("article", "panel-box");
    issue.append(node("p", "block-title", t("Issue a token")));
    const name = node("input", "field");
    name.type = "text";
    name.placeholder = t("what this token is for");
    issue.append(node("label", "field-label", t("Name")), name);
    const shown = node("pre", "copy");
    shown.hidden = true;
    const failure = node("p", "hint");
    const footer = node("p", "form-footer");
    const make = node("button", "btn", t("Issue"));
    make.onclick = async () => {
        try {
            const token = await call("POST", base() + "/tokens", {name: name.value});
            shown.hidden = false;
            shown.textContent = token.token;
            failure.textContent = "";
            decorateCopies(issue);
            none.hidden = true;
            rows.append(tokenRow({name: name.value, createdAt: Math.floor(Date.now() / 1000)}));
            name.value = "";
        } catch (error) {
            failure.textContent = error.message;
        }
    };
    footer.append(make);
    issue.append(footer, shown, failure);
    view.append(issue);

    const how = node("article", "panel-box");
    const howTitle = node("p", "block-title");
    howTitle.append(linkedText(t("Example request, more in the [documentation]"), kBotDocs));
    how.append(howTitle);
    how.append(node("pre", "copy",
        "curl -s http://" + location.host + "/v1/events?since=0\\&wait=60 \\\n  -H \"Authorization: Bearer $TOKEN\""));
    view.append(how);

    const issued = node("section", "stack-section");
    issued.append(node("p", "block-title", t("Issued tokens")), rows);
    view.append(issued);
    return view;
}

async function viewAccount() {
    const account = await call("GET", base());
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Account")));

    if (!account.running) {
        view.append(node("p", "empty",
            t("The account is not open: %1.", account.status)));
        return view;
    }

    const identity = node("article", "panel-box");
    identity.append(node("p", "block-title", t("Name")));
    const name = node("input", "field");
    name.type = "text";
    name.value = account.name || "";
    identity.append(name);
    identity.append(node("p", "hint",
        t("The name in the invite and in this panel.")));
    const renamed = node("p", "hint");
    const nameFooter = node("p", "form-footer");
    const saveName = node("button", "btn", t("Save"));
    saveName.onclick = async () => {
        try {
            await call("POST", base(), {action: "rename", name: name.value});
            renamed.textContent = t("Saved.");
            await drawSidebar();
        } catch (error) {
            renamed.textContent = error.message;
        }
    };
    nameFooter.append(saveName);
    identity.append(nameFooter, renamed);
    view.append(identity);

    const keys = node("article", "panel-box");
    keys.append(node("p", "block-title", t("Keys")));
    keys.append(node("p", "hint",
        t("A new address replaces this account's destination key. Invite links already handed out stop working. Contacts receive the new card.")));
    const keyResult = node("p", "hint");
    const keyFooter = node("p", "form-footer");
    const fresh = node("button", "btn btn-danger", t("New address"));
    fresh.onclick = async () => {
        if (!window.confirm(t("The old address stops answering. Continue?"))) {
            return;
        }
        keyResult.textContent = t("Creating a new address…");
        try {
            await call("POST", base(), {action: "new-address"});
            keyResult.textContent = t("Created. It is published once its tunnels are built.");
        } catch (error) {
            keyResult.textContent = error.message;
        }
    };
    keyFooter.append(fresh);
    keys.append(keyFooter, keyResult);

    const owed = account.pendingRouting || [];
    if (owed.length > 0 || account.pendingRegistry) {
        const stuck = node("div", "block");
        stuck.append(node("p", "block-title", t("The new card has not reached all contacts")));
        if (account.pendingRegistry) {
            stuck.append(node("p", "hint",
                t("The alias registry has not accepted the new address yet. It is retried every ten seconds.")));
        }
        for (const owedPeer of owed) {
            const row = node("p", "form-footer");
            row.append(node("span", "hint", owedPeer));
            const said = node("p", "hint");
            const again = node("button", "btn btn-quiet", t("Retry"));
            again.onclick = async () => {
                said.textContent = t("Sending…");
                try {
                    await call("POST", base(), {action: "retry-routing", peer: owedPeer});
                    said.textContent = t("Sent. The list clears on delivery.");
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

    view.append(exportCard(account));
    if (account.kind !== "group") {
        view.append(pairCard());
    }
    view.append(removeCard());
    return view;
}

function exportCard(account) {
    const move = node("article", "panel-box");
    move.append(node("p", "block-title", t("Back up this account")));
    const bundlePassword = node("input", "field");
    bundlePassword.type = "password";
    if (account.kind === "group") {
        move.append(node("p", "notice is-warning",
            t("Members, posts and group settings are not in the backup.")));
    }
    move.append(node("label", "field-label", t("Password for the backup")), bundlePassword);
    const wrote = node("p", "hint");
    const exportFooter = node("p", "form-footer");
    const run = node("button", "btn", t("Export"));
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
            wrote.textContent = t("Downloaded.");
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
    card.append(node("p", "block-title", t("Send to another device")));
    card.append(node("p", "hint",
        t("Bots do not support running on several devices. This action moves the account.")));
    const offer = node("div");
    offer.hidden = true;
    const link = node("pre", "copy");
    const code = node("p", "pair-code");
    offer.append(link, code);
    const status = node("p", "hint");
    const footer = node("p", "form-footer");
    const start = node("button", "btn", t("Start transfer"));
    const stop = node("button", "btn btn-quiet", t("Stop"));
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
            status.textContent = t("Publishing the transfer address…");
        } else if (state.stage === "waiting") {
            status.textContent = t("Waiting for the other device.");
        } else if (state.stage === "wrong-code") {
            status.textContent = t("Wrong codes entered: %1.", state.wrongCodes);
        } else if (state.stage === "sending") {
            status.textContent = t("Sending the account %1", progressText(state.done, state.total));
        } else if (state.stage === "done") {
            status.textContent = t("The account is on the other device.");
        } else if (state.stage === "refused") {
            status.textContent = t("Too many wrong codes. The transfer stopped.");
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
    card.append(node("p", "block-title", t("Remove this account")));
    card.append(node("p", "hint",
        t("Closes the account and deletes its file, mailbox, conversations and tokens. To keep the account, export it first.")));
    const said = node("p", "hint");
    const footer = node("p", "form-footer");
    const drop = node("button", "btn btn-danger", t("Remove"));
    drop.onclick = async () => {
        if (!window.confirm(t("Delete the account?"))) {
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
    view.append(node("h1", "view-title", t("Settings")));

    const transport = node("article", "panel-box");
    transport.append(node("p", "block-title", t("Transport")));
    transport.append(node("p", "hint",
        t("All accounts on this host use one router. An external router can be shared with other programs.")));
    const which = node("select", "field");
    for (const [value, label] of [["embedded", N_("Built-in router")],
        ["sam", N_("External router over SAM")]]) {
        const option = node("option", null, t(label));
        option.value = value;
        which.append(option);
    }
    which.value = settings.transport;
    transport.append(node("label", "field-label", t("I2P")), which);

    const samBlock = node("div", "block");
    samBlock.hidden = settings.transport !== "sam";
    const samHost = node("input", "field");
    samHost.type = "text";
    samHost.value = settings.samHost;
    const samPort = node("input", "field");
    samPort.type = "number";
    samPort.value = settings.samPort;
    samBlock.append(node("label", "field-label", t("SAM host")), samHost);
    samBlock.append(node("label", "field-label", t("SAM port")), samPort);
    transport.append(samBlock);
    which.onchange = () => { samBlock.hidden = which.value !== "sam"; };

    const privacy = node("select", "field");
    for (const [value, label] of [["minimal", N_("Minimal")], ["middle", N_("Middle")], ["max", N_("Max")]]) {
        const option = node("option", null, t(label));
        option.value = value;
        privacy.append(option);
    }
    privacy.value = settings.privacy;
    transport.append(node("label", "field-label", t("Tunnel length")), privacy);

    const saved = node("p", "hint");
    const footer = node("p", "form-footer");
    const save = node("button", "btn", t("Save"));
    save.onclick = async () => {
        try {
            await call("POST", "/ui/settings", {
                transport: which.value,
                samHost: samHost.value,
                samPort: Number(samPort.value),
                privacy: privacy.value,
            });
            saved.textContent = t("Saved. It takes effect at the next start.");
        } catch (error) {
            saved.textContent = error.message;
        }
    };
    footer.append(save);
    transport.append(footer, saved);
    view.append(transport);

    const operator = node("article", "panel-box");
    operator.append(node("p", "block-title", t("Administrator")));
    operator.append(node("p", "hint",
        t("The administrator fingerprint can be changed only in the configuration file, in the operator field.")));
    operator.append(facts([[t("Fingerprint"), settings.operator]]));
    view.append(operator);
    return view;
}

function segmented(options, value) {
    const group = node("div", "segmented");
    group.setAttribute("role", "radiogroup");
    let chosen = value;
    const buttons = options.map(([key, label, note]) => {
        const button = node("button", "segment" + (key === value ? " is-on" : ""));
        button.type = "button";
        button.setAttribute("role", "radio");
        button.setAttribute("aria-checked", key === value ? "true" : "false");
        button.append(node("span", "segment-label", t(label)));
        if (note) {
            button.append(node("span", "segment-note", t(note)));
        }
        button.onclick = () => {
            chosen = key;
            for (const other of buttons) {
                const on = other === button;
                other.classList.toggle("is-on", on);
                other.setAttribute("aria-checked", on ? "true" : "false");
            }
        };
        group.append(button);
        return button;
    });
    return {element: group, value: () => chosen};
}

function tally(pairs) {
    const line = node("dl", "tally");
    for (const [label, value] of pairs) {
        const item = node("div", "tally-item");
        item.append(node("dt", null, label), node("dd", null, String(value)));
        line.append(item);
    }
    return line;
}

function shortFingerprint(fingerprint) {
    return fingerprint.slice(0, 16) + "…";
}

async function viewGroup() {
    const state = await call("GET", base() + "/group");
    const config = state.config;
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Group")));
    view.append(tally([
        [t("Members"), state.states.member || 0],
        [t("Waiting for approval"), state.requests],
        [t("Blocked"), state.states.banned || 0],
    ]));

    const box = node("article", "panel-box");
    box.append(node("p", "block-title", t("Who writes")));
    const kind = segmented([
        ["group", N_("Group"), N_("Every member posts.")],
        ["channel", N_("Channel"), N_("Administrators post, members read.")],
    ], config.kind);
    box.append(kind.element);

    box.append(node("p", "block-title", t("Joining")));
    const joining = segmented([
        ["open", N_("Open"), N_("Anyone with the invite.")],
        ["approval", N_("With approval"), N_("An administrator approves each member.")],
    ], config.joinMode);
    box.append(joining.element);

    const history = node("input", "field field-narrow");
    history.type = "number";
    history.min = "0";
    history.value = String(config.historySize);
    box.append(node("label", "field-label", t("History for new members")), history);
    box.append(node("p", "hint", t("Posts a member receives on joining.")));

    const contactLine = node("label", "check");
    const contacts = node("input");
    contacts.type = "checkbox";
    contacts.checked = config.contactRequests;
    contactLine.append(contacts, node("span", null, t("Members may ask an author for contact")));
    box.append(contactLine);

    const botLanguage = node("select", "field field-narrow");
    for (const [code, name] of kLanguages) {
        const option = node("option", null, name);
        option.value = code;
        option.lang = code;
        botLanguage.append(option);
    }
    botLanguage.value = config.language;
    box.append(node("label", "field-label", t("Language of the bot's messages")), botLanguage);

    const greeting = node("textarea", "field");
    greeting.value = config.greeting;
    greeting.placeholder = config.greetingDefault;
    box.append(node("label", "field-label", t("Greeting")), greeting);
    const welcome = node("textarea", "field");
    welcome.value = config.welcome;
    welcome.placeholder = config.welcomeDefault;
    box.append(node("label", "field-label", t("After joining")), welcome);
    box.append(node("p", "hint", t("{name} is the member's name.")));

    const said = node("p", "hint");
    const footer = node("p", "form-footer");
    const save = node("button", "btn", t("Save"));
    save.onclick = async () => {
        save.disabled = true;
        said.textContent = "";
        try {
            await call("POST", base() + "/group", {
                kind: kind.value(),
                joinMode: joining.value(),
                historySize: Number(history.value),
                contactRequests: contacts.checked,
                language: botLanguage.value,
                greeting: greeting.value.trim(),
                welcome: welcome.value.trim(),
            });
            said.textContent = t("Saved.");
        } catch (error) {
            said.textContent = error.message;
        }
        save.disabled = false;
    };
    footer.append(save);
    box.append(footer, said);
    view.append(box);
    return view;
}

function inFilter(member, filter) {
    if (filter === "all") {
        return true;
    }
    if (filter === "admin") {
        return member.admin;
    }
    if (filter === "left") {
        return member.state === "left" || member.state === "gone";
    }
    return member.state === filter;
}

function memberMatches(member) {
    if (!inFilter(member, memberFilter)) {
        return false;
    }
    const asked = memberQuery.trim().toLowerCase();
    return !asked || member.name.toLowerCase().includes(asked)
        || member.fingerprint.includes(asked);
}

function memberRow(member, refresh) {
    const row = node("div", "row is-static");
    const keys = node("span", "row-keys");
    keys.append(node("span", "row-key", member.name || t("no name yet")));
    const fingerprint = node("span", "row-dest", shortFingerprint(member.fingerprint));
    fingerprint.dataset.copy = member.fingerprint;
    keys.append(fingerprint);
    const tags = node("span", "row-tags");
    if (member.admin) {
        tags.append(node("span", "tag is-key", t("administrator")));
    }
    const stateClass = {pending: " is-pending", banned: " is-banned", member: ""}[member.state];
    if (member.state !== "member") {
        tags.append(node("span", "tag" + (stateClass || ""), t(kStateLabels[member.state])));
    }
    if (member.joinedAt) {
        tags.append(node("span", "row-dest", new Date(member.joinedAt).toLocaleDateString()));
    }
    const actions = node("span", "row-actions");
    const act = (label, action, danger, ask) => {
        const button = node("button", "btn btn-small" + (danger ? " btn-danger" : " btn-quiet"),
            t(label));
        button.type = "button";
        button.onclick = async () => {
            if (ask && !window.confirm(ask)) {
                return;
            }
            button.disabled = true;
            try {
                await call("POST", base() + "/group/members",
                    {fingerprint: member.fingerprint, action});
                await refresh();
            } catch (error) {
                button.disabled = false;
                button.title = error.message;
            }
        };
        actions.append(button);
    };
    if (member.state === "member") {
        act(member.admin ? N_("Remove administrator") : N_("Make administrator"),
            member.admin ? "unadmin" : "admin", false);
        if (!member.admin) {
            act(N_("Exclude"), "exclude", true);
        }
    } else if (member.state === "banned") {
        act(N_("Unblock"), "unblock", false);
    } else {
        act(N_("Block"), "block", true, t("Block %1? Returning will not be possible.", member.name));
    }
    row.append(keys, tags, actions);
    return row;
}

async function viewMembers() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Members")));
    const toolbar = node("div", "toolbar");
    const search = node("input", "field search");
    search.type = "search";
    search.placeholder = t("Name or fingerprint");
    search.value = memberQuery;
    const filters = node("div", "filters");
    toolbar.append(search, filters);
    const rows = node("div", "rows");
    view.append(toolbar, rows);

    let members = [];
    const draw = () => {
        filters.textContent = "";
        for (const [key, label] of kMemberFilters) {
            const count = members.filter((member) => inFilter(member, key)).length;
            const chip = node("button", "chip" + (memberFilter === key ? " is-on" : ""),
                t(label) + " " + count);
            chip.type = "button";
            chip.setAttribute("aria-pressed", memberFilter === key ? "true" : "false");
            chip.onclick = () => {
                memberFilter = key;
                draw();
            };
            filters.append(chip);
        }
        rows.textContent = "";
        const shown = members.filter(memberMatches);
        if (members.length === 0) {
            rows.append(node("p", "empty", t("Nobody has joined yet.")));
        } else if (shown.length === 0) {
            rows.append(node("p", "empty", t("Nobody matches.")));
        }
        for (const member of shown) {
            rows.append(memberRow(member, refresh));
        }
        decorateCopies(rows);
    };
    const refresh = async () => {
        members = await call("GET", base() + "/group/members");
        members.sort((a, b) => (b.admin - a.admin) || a.name.localeCompare(b.name));
        draw();
    };
    search.oninput = () => {
        memberQuery = search.value;
        draw();
    };
    await refresh();
    return view;
}

async function viewRequests() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Requests")));
    const state = await call("GET", base() + "/group");
    const rows = node("div", "rows");
    view.append(rows);
    const refresh = async () => {
        const asked = await call("GET", base() + "/group/requests");
        rows.textContent = "";
        if (asked.length === 0) {
            rows.append(node("p", "empty", state.config.joinMode === "open"
                ? t("Joining is open.")
                : t("Nobody is waiting.")));
        }
        for (const request of asked) {
            const row = node("div", "row is-static");
            const keys = node("span", "row-keys");
            keys.append(node("span", "row-key", request.name));
            const fingerprint = node("span", "row-dest", shortFingerprint(request.fingerprint));
            fingerprint.dataset.copy = request.fingerprint;
            keys.append(fingerprint);
            const tags = node("span", "row-tags");
            tags.append(node("span", "row-dest", when(request.at)));
            const actions = node("span", "row-actions");
            for (const [label, allow, style] of [[N_("Allow"), true, "btn"],
                [N_("Decline"), false, "btn btn-quiet"]]) {
                const button = node("button", style + " btn-small", t(label));
                button.type = "button";
                button.onclick = async () => {
                    button.disabled = true;
                    await call("POST", base() + "/group/requests", {number: request.number, allow});
                    await refresh();
                };
                actions.append(button);
            }
            row.append(keys, tags, actions);
            rows.append(row);
        }
        decorateCopies(rows);
    };
    await refresh();
    return view;
}

function postBody(post) {
    if (post.type === "image") {
        return t("Picture");
    }
    if (post.type === "audio") {
        return t("Voice message");
    }
    return post.text;
}

async function viewPosts() {
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("Posts")));
    const list = node("div", "posts");
    view.append(list);
    const refresh = async () => {
        const posts = await call("GET", base() + "/group/posts?limit=" + kPostsPage);
        list.textContent = "";
        if (posts.length === 0) {
            list.append(node("p", "empty", t("No posts yet.")));
        }
        for (const post of posts) {
            const item = node("article", "post" + (post.deleted ? " is-deleted" : ""));
            const head = node("header", "post-head");
            head.append(node("span", "post-author", post.authorName || t("no name yet")));
            head.append(node("time", "post-time", when(post.sentAt)));
            if (post.edited) {
                head.append(node("span", "tag", t("edited")));
            }
            if (post.deleted) {
                head.append(node("span", "tag is-banned", t("deleted")));
            }
            item.append(head);
            item.append(node("p", "post-body" + (post.type === "text" ? "" : " is-media"),
                postBody(post)));
            const foot = node("footer", "post-foot");
            const reactions = node("span", "reactions");
            for (const [emoji, count] of Object.entries(post.reactions)) {
                reactions.append(node("span", "reaction", emoji + " " + count));
            }
            foot.append(reactions);
            if (!post.deleted) {
                const drop = node("button", "btn btn-small btn-quiet btn-danger-quiet", t("Delete"));
                drop.type = "button";
                drop.onclick = async () => {
                    if (!window.confirm(t("Delete this post for every member?"))) {
                        return;
                    }
                    drop.disabled = true;
                    await call("DELETE", base() + "/group/posts/" + encodeURIComponent(post.id));
                    await refresh();
                };
                foot.append(drop);
            }
            item.append(foot);
            list.append(item);
        }
    };
    await refresh();
    return view;
}

const kUploadChunkBytes = 1024 * 1024;
const kBytesStep = 1024;
const kByteUnits = [N_("%1 B"), N_("%1 KiB"), N_("%1 MiB"), N_("%1 GiB")];

function bytesText(bytes) {
    let value = bytes;
    let unit = 0;
    while (value >= kBytesStep && unit < kByteUnits.length - 1) {
        value /= kBytesStep;
        ++unit;
    }
    return t(kByteUnits[unit], unit === 0 ? value : value.toFixed(1));
}

const kSecondsPerMinute = 60;
const kSecondsPerHour = 60 * kSecondsPerMinute;
const kSecondsPerDay = 24 * kSecondsPerHour;

function uptimeText(seconds) {
    const days = Math.floor(seconds / kSecondsPerDay);
    const hours = Math.floor(seconds % kSecondsPerDay / kSecondsPerHour);
    const minutes = Math.floor(seconds % kSecondsPerHour / kSecondsPerMinute);
    return days > 0 ? t("%1 d %2 h", days, hours) : t("%1 h %2 min", hours, minutes);
}

async function viewSystem() {
    const system = await call("GET", "/ui/system");
    const view = node("section", "view");
    view.append(node("h1", "view-title", t("System")));

    const router = node("article", "panel-box");
    router.append(node("p", "block-title", system.transport === "sam"
        ? t("External router over SAM") : t("Built-in router")));
    const routed = [];
    if (system.router.counted) {
        const links = system.router.connections || {};
        routed.push([t("Tunnels in / out"),
            system.router.inboundTunnels + " / " + system.router.outboundTunnels]);
        routed.push([t("Transit tunnels"), String(system.router.transitTunnels)]);
        routed.push([t("Known routers / floodfills"),
            system.router.knownRouters + " / " + system.router.floodfills]);
        routed.push([t("Connections"), Object.keys(links).length === 0
            ? "0" : Object.entries(links).map(([kind, count]) => kind + " " + count).join(", ")]);
    }
    routed.push([t("Destinations ready / all"),
        system.router.readyDestinations + " / " + system.router.destinations]);
    router.append(facts(routed, false));
    view.append(router);

    const service = node("article", "panel-box");
    service.append(node("p", "block-title", t("Service")));
    service.append(facts([
        [t("Version"), system.version],
        [t("Uptime"), uptimeText(system.uptimeSeconds)],
        [t("Memory now / peak"),
            bytesText(system.memory.residentBytes) + " / " + bytesText(system.memory.peakBytes)],
        [t("Data on disk"), bytesText(system.disk.stateBytes)],
        [t("Free on the disk"), t("%1 of %2", bytesText(system.disk.freeBytes),
            bytesText(system.disk.totalBytes))],
    ], false));
    view.append(service);

    view.append(systemBackupCard());
    return view;
}

function systemBackupCard() {
    const box = node("article", "panel-box");
    box.append(node("p", "block-title", t("Backup")));
    box.append(node("p", "hint",
        t("An archive of all service data.")));
    const exportFooter = node("p", "form-footer");
    const download = node("a", "btn", t("Export"));
    download.href = "/ui/system/backup";
    download.setAttribute("download", "");
    exportFooter.append(download);
    box.append(exportFooter);

    const archive = node("input", "field");
    archive.type = "file";
    archive.accept = ".zip";
    box.append(node("label", "field-label", t("Archive to import")), archive);
    box.append(node("p", "notice is-warning",
        t("Importing replaces all service data except the administrator fingerprint.")));
    const state = node("p", "hint");
    const importFooter = node("p", "form-footer");
    const restore = node("button", "btn btn-danger", t("Import"));
    restore.onclick = async () => {
        const file = archive.files && archive.files[0];
        if (!file) {
            state.textContent = t("Pick the archive first.");
            return;
        }
        if (!confirm(t("Replace all service data?"))) {
            return;
        }
        restore.disabled = true;
        try {
            for (let offset = 0; offset < file.size; offset += kUploadChunkBytes) {
                const piece = await file.slice(offset, offset + kUploadChunkBytes).arrayBuffer();
                const sent = await fetch("/ui/system/restore?offset=" + offset, {method: "POST",
                    headers: {"Content-Type": "application/octet-stream"}, body: piece});
                if (!sent.ok) {
                    throw new Error((await sent.json()).error || sent.statusText);
                }
                state.textContent = t("Uploading %1", progressText(offset + piece.byteLength, file.size));
            }
        } catch (error) {
            restore.disabled = false;
            state.textContent = error.message;
            return;
        }
        let committed = null;
        try {
            committed = await fetch("/ui/system/restore/commit", {method: "POST"});
        } catch (error) {
            committed = null;
        }
        if (committed && !committed.ok) {
            restore.disabled = false;
            state.textContent = (await committed.json()).error || committed.statusText;
            return;
        }
        state.textContent = t("Restoring and restarting…");
        let wentDown = false;
        for (;;) {
            await new Promise((resume) => setTimeout(resume, kRetryMs));
            try {
                const answered = await fetch("/ui/state");
                if (wentDown && answered.ok) {
                    location.reload();
                    return;
                }
            } catch (error) {
                wentDown = true;
            }
        }
    };
    importFooter.append(restore);
    box.append(importFooter, state);
    return box;
}

const views = {
    overview: viewOverview,
    chats: viewChats,
    contacts: viewContacts,
    aliases: viewAliases,
    sign: viewSign,
    api: viewApi,
    account: viewAccount,
    group: viewGroup,
    members: viewMembers,
    requests: viewRequests,
    posts: viewPosts,
    add: viewAdd,
    settings: viewSettings,
    system: viewSystem,
};

async function render() {
    polling++;
    try {
        accounts = await call("GET", "/ui/accounts");
        const route = currentRoute();
        if (route.account !== accountId) {
            accountId = route.account;
            menuOpen = true;
            peer = null;
            editing = null;
            names = {};
            lastEventId = 0;
        }
        await drawSidebar();
        const summary = accounts.find((account) => account.id === accountId);
        const needsOpen = ["chats", "contacts", "aliases", "sign", "api", "group", "members",
            "requests", "posts"].includes(route.view);
        if (needsOpen && summary && !summary.running) {
            const waiting = node("section", "view");
            waiting.append(node("h1", "view-title", viewLabel(route.view)));
            waiting.append(node("p", "empty",
                t("The account is not open: %1.", summary.status)));
            content().replaceChildren(waiting);
            return;
        }
        if (summary && summary.running && summary.kind !== "group") {
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
        failed.append(node("h1", "view-title", t("Something went wrong")));
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

el("corner").append(languagePicker(render));

el("signOut").onclick = async () => {
    await call("POST", "/ui/logout");
    location.href = "/";
};

window.addEventListener("hashchange", render);
render();
call("GET", "/ui/state").then((state) => {
    el("version").textContent = state.version || "";
});
