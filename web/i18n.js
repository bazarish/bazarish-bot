// Bazarish project (c) 2026
"use strict";

const kLanguages = [
    ["en", "English"],
    ["ru", "Русский"],
    ["es", "Español"],
    ["zh", "中文"],
    ["ar", "العربية"],
];
const kRightToLeft = ["ar"];
const kLanguageKey = "bazarish-bot-language";

function knownLanguage(code) {
    return kLanguages.some(([known]) => known === code);
}

function chosenLanguage() {
    try {
        const kept = localStorage.getItem(kLanguageKey);
        if (kept && knownLanguage(kept)) {
            return kept;
        }
    } catch (error) {
        return "en";
    }
    const asked = (navigator.language || "en").slice(0, 2);
    return knownLanguage(asked) ? asked : "en";
}

let language = chosenLanguage();

function N_(text) {
    return text;
}

function t(text, ...values) {
    const table = language === "en" ? null : (window.bazarishMessages || {})[language];
    let out = table && table[text] ? table[text] : text;
    values.forEach((value, index) => {
        out = out.split("%" + (index + 1)).join(String(value));
    });
    return out;
}

function applyLanguage() {
    document.documentElement.lang = language;
    document.documentElement.dir = kRightToLeft.includes(language) ? "rtl" : "ltr";
    for (const marked of document.querySelectorAll("[data-t]")) {
        marked.textContent = t(marked.dataset.t);
    }
}

function setLanguage(code) {
    language = code;
    applyLanguage();
    try {
        localStorage.setItem(kLanguageKey, code);
    } catch (error) {
        return;
    }
}

function languagePicker(changed) {
    const picker = document.createElement("select");
    picker.className = "field field-language";
    picker.setAttribute("aria-label", t("Language"));
    for (const [code, name] of kLanguages) {
        const option = document.createElement("option");
        option.value = code;
        option.textContent = name;
        option.lang = code;
        picker.append(option);
    }
    picker.value = language;
    picker.onchange = () => {
        setLanguage(picker.value);
        changed();
    };
    return picker;
}

applyLanguage();
