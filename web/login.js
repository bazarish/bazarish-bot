// Bazarish project (c) 2026
"use strict";

const el = (id) => document.getElementById(id);

let challenge = "";

async function start() {
    const state = await (await fetch("/ui/state")).json();
    if (state.signedIn) {
        location.href = "/";
        return;
    }
    challenge = state.challenge;
    el("challenge").textContent = challenge;
    el("unclaimed").hidden = state.claimed;
    el("title").textContent = state.claimed ? "Sign in" : "Claim this host";
    el("signIn").textContent = state.claimed ? "Sign in" : "Claim";
}

el("signIn").onclick = async () => {
    try {
        const answer = await fetch("/ui/login", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({challenge, blob: el("blob").value.trim()}),
        });
        const parsed = await answer.json();
        if (!answer.ok) {
            throw new Error(parsed.error || answer.statusText);
        }
        location.href = "/";
    } catch (error) {
        el("signinError").textContent = error.message;
        await start();
    }
};

el("blob").addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        el("signIn").click();
    }
});

start();
