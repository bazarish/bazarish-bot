// Bazarish project (c) 2026
(function () {
    "use strict";

    const kStyleMarkers = [["**", "bold"], ["~~", "strike"], ["*", "italic"]];
    const kSendMarker = "!!";
    const kCodeMarker = "```";
    const kAliasMarker = "!";
    const kAliasMinLength = 1;
    const kAliasMaxLength = 16;
    const kLinkSchemes = ["https://", "http://"];
    const kTrailingPunctuation = ".,;:!?'\"»";
    const kBrackets = [["(", ")"], ["[", "]"], ["{", "}"]];
    const kInvite = /bazarish:\/\/invite\?\S+/g;
    const kInvitePrefix = "bazarish://invite?";
    const kFingerprintShown = 16;

    const isWordChar = (c) => c !== undefined && /[\p{L}\p{N}]/u.test(c);
    const isSpace = (c) => c !== undefined && /\s/u.test(c);
    const isAliasChar = (c) => c !== undefined && /[A-Za-z0-9]/.test(c);
    const isNameSeparator = (c) => c === "_" || c === "." || c === "-";

    function parse(text) {
        const runs = [];
        const lineEnd = (from, to) => {
            const found = text.indexOf("\n", from);
            return found < 0 || found > to ? to : found;
        };
        const matchesAt = (at, to, what) => at + what.length <= to && text.startsWith(what, at);

        function matchStyle(at, to) {
            const limit = lineEnd(at, to);
            for (const [marker, kind] of kStyleMarkers) {
                const length = marker.length;
                if (!matchesAt(at, limit, marker)) {
                    continue;
                }
                if (at > 0 && isWordChar(text[at - 1])) {
                    continue;
                }
                if (at + length >= limit || isSpace(text[at + length])) {
                    continue;
                }
                for (let j = at + length + 1; j + length <= limit; ++j) {
                    if (!matchesAt(j, limit, marker) || isSpace(text[j - 1])) {
                        continue;
                    }
                    if (j + length < to && isWordChar(text[j + length])) {
                        continue;
                    }
                    return {kind, from: at + length, to: j, after: j + length};
                }
            }
            return null;
        }

        function matchPaired(at, to, marker) {
            if (!matchesAt(at, to, marker)) {
                return null;
            }
            const length = marker.length;
            for (let j = at + length; j + length <= to; ++j) {
                if (!matchesAt(j, to, marker)) {
                    continue;
                }
                if (text.slice(at + length, j).trim() === "") {
                    return null;
                }
                return {from: at + length, to: j, after: j + length};
            }
            return null;
        }

        function trimTail(from, end) {
            let trimmed = true;
            while (trimmed && end > from) {
                trimmed = false;
                const last = text[end - 1];
                if (kTrailingPunctuation.includes(last)) {
                    --end;
                    trimmed = true;
                    continue;
                }
                for (const [open, close] of kBrackets) {
                    if (last !== close) {
                        continue;
                    }
                    const address = text.slice(from, end);
                    if (address.split(close).length > address.split(open).length) {
                        --end;
                        trimmed = true;
                    }
                    break;
                }
            }
            return end;
        }

        function matchLink(at, to) {
            if (at > 0 && isWordChar(text[at - 1])) {
                return null;
            }
            const limit = lineEnd(at, to);
            let start = -1;
            for (const scheme of kLinkSchemes) {
                if (at + scheme.length <= limit
                    && text.slice(at, at + scheme.length).toLowerCase() === scheme) {
                    start = at + scheme.length;
                    break;
                }
            }
            if (start < 0 || start >= limit || !isWordChar(text[start])) {
                return null;
            }
            let end = start;
            while (end < limit && !isSpace(text[end]) && !"<>\"".includes(text[end])) {
                ++end;
            }
            end = trimTail(at, end);
            return end > start ? {after: end} : null;
        }

        function matchAlias(at, to) {
            if (text[at] !== kAliasMarker) {
                return null;
            }
            if (at > 0 && (isWordChar(text[at - 1]) || text[at - 1] === kAliasMarker)) {
                return null;
            }
            let end = at + 1;
            while (end < to && isAliasChar(text[end])) {
                ++end;
            }
            const length = end - at - 1;
            if (length < kAliasMinLength || length > kAliasMaxLength) {
                return null;
            }
            if (end < to && isWordChar(text[end])) {
                return null;
            }
            if (end + 1 < to && isNameSeparator(text[end]) && isAliasChar(text[end + 1])) {
                return null;
            }
            return {after: end};
        }

        function scan(from, to, style, allowStyles) {
            let buffer = "";
            const flush = () => {
                if (buffer) {
                    runs.push({text: buffer, action: "none", ...style});
                    buffer = "";
                }
            };
            const emit = (piece, action) => runs.push({text: piece, action, ...style});
            let i = from;
            while (i < to) {
                const styled = allowStyles ? matchStyle(i, to) : null;
                if (styled) {
                    flush();
                    scan(styled.from, styled.to, {...style, [styled.kind]: true}, false);
                    i = styled.after;
                    continue;
                }
                const code = matchPaired(i, to, kCodeMarker);
                if (code) {
                    flush();
                    emit(text.slice(code.from, code.to), "code");
                    i = code.after;
                    continue;
                }
                const send = matchPaired(i, lineEnd(i, to), kSendMarker);
                if (send) {
                    flush();
                    emit(text.slice(send.from, send.to), "send");
                    i = send.after;
                    continue;
                }
                const link = matchLink(i, to);
                if (link) {
                    flush();
                    emit(text.slice(i, link.after), "link");
                    i = link.after;
                    continue;
                }
                const alias = matchAlias(i, to);
                if (alias) {
                    flush();
                    emit(text.slice(i, alias.after), "alias");
                    i = alias.after;
                    continue;
                }
                buffer += text[i];
                ++i;
            }
            flush();
        }

        scan(0, text.length, {}, true);
        return runs;
    }

    function invites(text) {
        const found = [];
        for (const match of text.matchAll(kInvite)) {
            const fields = {};
            for (const pair of match[0].slice(kInvitePrefix.length).split("&")) {
                const eq = pair.indexOf("=");
                if (eq > 0) {
                    try {
                        fields[pair.slice(0, eq)] = decodeURIComponent(pair.slice(eq + 1));
                    } catch (error) {
                        fields[pair.slice(0, eq)] = "";
                    }
                }
            }
            if (fields.fp && fields.dest) {
                found.push({uri: match[0], name: fields.name || "", fingerprint: fields.fp});
            }
        }
        return found;
    }

    const kClasses = {code: "markup-code", send: "markup-chip", alias: "markup-chip",
        link: "markup-link"};

    function runNode(run) {
        let made = document.createTextNode(run.text);
        for (const [flag, tag] of [["bold", "b"], ["italic", "i"], ["strike", "s"]]) {
            if (run[flag] && run.action !== "code") {
                const wrap = document.createElement(tag);
                wrap.append(made);
                made = wrap;
            }
        }
        if (run.action === "none") {
            return made;
        }
        if (run.action === "code") {
            const lines = document.createDocumentFragment();
            run.text.split("\n").forEach((line, index) => {
                if (index > 0) {
                    lines.append("\n");
                }
                if (line) {
                    const code = document.createElement("code");
                    code.className = kClasses.code;
                    code.textContent = line;
                    lines.append(code);
                }
            });
            return lines;
        }
        const span = document.createElement("span");
        span.className = kClasses[run.action];
        span.append(made);
        return span;
    }

    function draw(into, text, contactLabel) {
        into.textContent = "";
        const plates = invites(text);
        for (const plate of plates) {
            const card = document.createElement("div");
            card.className = "invite-plate";
            const name = document.createElement("b");
            name.textContent = plate.name || contactLabel;
            const print = document.createElement("span");
            print.textContent = plate.fingerprint.slice(0, kFingerprintShown) + "…";
            card.append(name, print);
            into.append(card);
        }
        let rest = text;
        for (const plate of plates) {
            rest = rest.replace(plate.uri, "");
        }
        rest = plates.length > 0 ? rest.trim() : rest;
        if (rest) {
            const body = document.createElement("div");
            body.className = "markup-body";
            for (const run of parse(rest)) {
                body.append(runNode(run));
            }
            into.append(body);
        }
    }

    window.bazarishMarkup = {draw};
})();
