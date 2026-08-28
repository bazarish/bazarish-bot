// Bazarish project (c) 2026
// The interface: one page over the /ui routes. It holds a long poll open for
// new lines rather than asking on a timer, the same way the API serves a bot.
"use strict";

const el = (id) => document.getElementById(id);
const show = (node, visible) => node.classList.toggle("hidden", !visible);

let peer = null;
let lastEventId = 0;
let polling = false;

async function call(method, path, body) {
    const options = { method, headers: {} };
    if (body !== undefined) {
        options.headers["Content-Type"] = "application/json";
        options.body = JSON.stringify(body);
    }
    const response = await fetch(path, options);
    const text = await response.text();
    const parsed = text ? JSON.parse(text) : {};
    if (!response.ok) {
        throw new Error(parsed.error || response.statusText);
    }
    return parsed;
}

function say(node, message, isError) {
    node.textContent = message;
    node.className = isError ? "error" : "ok";
}

async function refresh() {
    const state = await call("GET", "/ui/state");
    el("status").textContent = state.status;
    show(el("setupPanel"), !state.hasPassword);
    show(el("loginPanel"), state.hasPassword && !state.signedIn);
    show(el("signOut"), state.signedIn);
    show(el("accountPanel"), state.signedIn && !state.hasAccount);
    show(el("unlockPanel"), state.signedIn && state.hasAccount && !state.running);
    show(el("running"), state.signedIn && state.running);
    if (state.signedIn && state.running) {
        await loadRunning();
    }
}

async function loadRunning() {
    const account = await call("GET", "/ui/account");
    el("accName").textContent = account.name || "(unnamed)";
    el("accFp").textContent = account.fingerprint;
    el("accDest").textContent = account.destination || "building…";
    el("accInvite").textContent = account.invite || "";
    await Promise.all([loadChats(), loadContacts(), loadSettings()]);
    if (!polling) {
        polling = true;
        lastEventId = 0;
        pollEvents();
    }
}

async function loadChats() {
    const chats = await call("GET", "/ui/chats");
    const list = el("chatList");
    list.innerHTML = "";
    for (const chat of chats) {
        const item = document.createElement("div");
        item.className = "item" + (chat.peer === peer ? " active" : "");
        const name = document.createElement("div");
        name.className = "name";
        name.textContent = chat.peer.slice(0, 16);
        const preview = document.createElement("div");
        preview.className = "preview";
        preview.textContent = previewOf(chat);
        item.append(name, preview);
        item.onclick = () => openChat(chat.peer);
        list.append(item);
    }
    if (!peer && chats.length > 0) {
        await openChat(chats[0].peer);
    }
}

function previewOf(line) {
    if (line.type === "text" || line.type === "contact.request") {
        return (line.outgoing ? "you: " : "") + line.text;
    }
    return "[" + line.type + "]";
}

async function openChat(which) {
    peer = which;
    const lines = await call("GET", "/ui/messages?peer=" + encodeURIComponent(which) + "&limit=100");
    const box = el("lines");
    box.innerHTML = "";
    for (const line of lines.slice().reverse()) {
        box.append(bubbleFor(line));
    }
    box.scrollTop = box.scrollHeight;
    await loadChats();
}

function bubbleFor(line) {
    const bubble = document.createElement("div");
    bubble.className = "bubble" + (line.outgoing ? " out" : "");
    bubble.textContent = line.type === "text" ? line.text : "[" + line.type + "] " + (line.text || "");
    const when = document.createElement("span");
    when.className = "when";
    when.textContent = line.sentAt ? new Date(line.sentAt).toLocaleString() : "";
    bubble.append(when);
    return bubble;
}

async function loadContacts() {
    const contacts = await call("GET", "/ui/contacts");
    const table = el("contacts");
    table.innerHTML = "<tr><th>Name</th><th>Fingerprint</th><th></th></tr>";
    for (const contact of contacts) {
        const row = table.insertRow();
        row.insertCell().textContent = contact.name || "(unnamed)";
        const fingerprint = row.insertCell();
        fingerprint.textContent = contact.fingerprint;
        fingerprint.className = "mono-break";
        row.insertCell().textContent = contact.pending ? "pending" : "";
    }
}

async function loadSettings() {
    const settings = await call("GET", "/ui/settings");
    el("transport").value = settings.transport;
    el("samHost").value = settings.samHost;
    el("samPort").value = settings.samPort;
    el("privacy").value = settings.privacy;
    show(el("samFields"), settings.transport === "sam");
    const table = el("tokens");
    table.innerHTML = "<tr><th>Name</th><th>Issued</th><th></th></tr>";
    for (const token of settings.tokens) {
        const row = table.insertRow();
        row.insertCell().textContent = token.name;
        row.insertCell().textContent = new Date(token.createdAt * 1000).toLocaleDateString();
        const actions = row.insertCell();
        const revoke = document.createElement("button");
        revoke.textContent = "Revoke";
        revoke.onclick = async () => {
            await call("DELETE", "/ui/tokens/" + encodeURIComponent(token.name));
            await loadSettings();
        };
        actions.append(revoke);
    }
}

// New lines arrive on a request the daemon holds open, so an idle page is idle.
async function pollEvents() {
    while (polling) {
        try {
            const answer = await call("GET", "/ui/events?wait=30&since=" + lastEventId);
            lastEventId = answer.lastEventId || lastEventId;
            if (answer.events.length > 0) {
                await loadChats();
                if (peer) {
                    await openChat(peer);
                }
            }
        } catch (error) {
            // A daemon that is restarting answers nothing for a moment; try again
            // rather than leaving the page silently stale.
            await new Promise((resume) => setTimeout(resume, 3000));
        }
    }
}

el("setupSubmit").onclick = async () => {
    try {
        await call("POST", "/ui/password", { password: el("setupPassword").value });
        await call("POST", "/ui/login", { password: el("setupPassword").value });
        await refresh();
    } catch (error) {
        say(el("setupError"), error.message, true);
    }
};

el("loginSubmit").onclick = async () => {
    try {
        await call("POST", "/ui/login", { password: el("loginPassword").value });
        el("loginPassword").value = "";
        await refresh();
    } catch (error) {
        say(el("loginError"), error.message, true);
    }
};

el("signOut").onclick = async () => {
    await call("POST", "/ui/logout");
    location.reload();
};

el("accountImportToggle").onclick = () => show(el("importFields"), true);

el("accountCreate").onclick = async () => {
    try {
        await call("POST", "/ui/account", { action: "create", name: el("accountName").value,
            passphrase: el("accountPassphrase").value });
        await refresh();
    } catch (error) {
        say(el("accountError"), error.message, true);
    }
};

el("accountImport").onclick = async () => {
    try {
        await call("POST", "/ui/account", { action: "import", bundle: el("importBundle").value,
            bundlePassphrase: el("importPassword").value,
            passphrase: el("accountPassphrase").value });
        await refresh();
    } catch (error) {
        say(el("accountError"), error.message, true);
    }
};

el("unlockSubmit").onclick = async () => {
    try {
        await call("POST", "/ui/account", { action: "open",
            passphrase: el("unlockPassphrase").value });
        el("unlockPassphrase").value = "";
        await refresh();
    } catch (error) {
        say(el("unlockError"), error.message, true);
    }
};

el("exportRun").onclick = async () => {
    try {
        const done = await call("POST", "/ui/account", { action: "export",
            bundle: el("exportPath").value, bundlePassphrase: el("exportPassword").value });
        say(el("exportResult"), "written to " + done.written, false);
    } catch (error) {
        say(el("exportResult"), error.message, true);
    }
};

el("composer").onsubmit = async (event) => {
    event.preventDefault();
    const text = el("composerText").value;
    if (!text || !peer) {
        return;
    }
    el("composerText").value = "";
    await call("POST", "/ui/messages", { peer, text });
    await openChat(peer);
};

el("inviteAdd").onclick = async () => {
    try {
        await call("POST", "/ui/contacts", { invite: el("inviteUri").value,
            text: el("inviteText").value });
        await loadContacts();
    } catch (error) {
        say(el("contactError"), error.message, true);
    }
};

el("transport").onchange = () => show(el("samFields"), el("transport").value === "sam");

el("settingsSave").onclick = async () => {
    try {
        await call("POST", "/ui/settings", {
            transport: el("transport").value,
            samHost: el("samHost").value,
            samPort: Number(el("samPort").value),
            privacy: el("privacy").value,
        });
        say(el("settingsError"), "saved; it takes effect at the next start", false);
    } catch (error) {
        say(el("settingsError"), error.message, true);
    }
};

refresh().catch((error) => say(el("status"), error.message, true));
