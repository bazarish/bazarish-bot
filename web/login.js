// Bazarish project (c) 2026
// The sign-in page: it asks for a password, or for a first one when the daemon
// has never been set up.
"use strict";

const el = (id) => document.getElementById(id);

async function call(path, body) {
    const answer = await fetch(path, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
    });
    const text = await answer.text();
    const parsed = text ? JSON.parse(text) : {};
    if (!answer.ok) {
        throw new Error(parsed.error || answer.statusText);
    }
    return parsed;
}

async function start() {
    const state = await (await fetch("/ui/state")).json();
    el("setup").hidden = state.hasPassword;
    el("signin").hidden = !state.hasPassword;
}

el("setPassword").onclick = async () => {
    try {
        const password = el("newPassword").value;
        await call("/ui/password", {password});
        await call("/ui/login", {password});
        location.href = "/";
    } catch (error) {
        el("setupError").textContent = error.message;
    }
};

el("signIn").onclick = async () => {
    try {
        await call("/ui/login", {password: el("password").value});
        location.href = "/";
    } catch (error) {
        el("signinError").textContent = error.message;
    }
};

for (const field of ["newPassword", "password"]) {
    el(field).addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
            el(field === "password" ? "signIn" : "setPassword").click();
        }
    });
}

start();
