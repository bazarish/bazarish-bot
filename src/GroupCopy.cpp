// Bazarish project (c) 2026
#include "GroupCopy.hpp"

#include <array>
#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace bazarish::bot {

namespace {

constexpr std::size_t kLanguageCount = std::size(kGroupLanguages);

struct Entry {
    Copy key;
    std::array<const char*, kLanguageCount> texts;
};

const Entry kEntries[] = {
    {Copy::eAskName, {"Choose your name in this chat.", "Выберите имя в этом чате.", "Elige tu nombre en este chat.", "请为自己在这个聊天中取一个名字。", "اختر اسمك في هذه الدردشة."}},
    {Copy::eNameTaken, {"That name is taken. Choose another.", "Имя занято. Выберите другое.", "Ese nombre está ocupado. Elige otro.", "这个名字已被占用，请换一个。", "هذا الاسم مأخوذ. اختر اسمًا آخر."}},
    {Copy::eNameRefused, {"That name is not allowed. Choose another.", "Такое имя недопустимо. Выберите другое.", "Ese nombre no está permitido. Elige otro.", "这个名字不可用，请换一个。", "هذا الاسم غير مسموح. اختر اسمًا آخر."}},
    {Copy::eAwaitingApproval, {"Your request is waiting for an admin.", "Заявка ждёт решения админа.", "Tu solicitud espera a un administrador.", "你的申请正在等待管理员处理。", "طلبك بانتظار أحد المشرفين."}},
    {Copy::eWelcome, {"Welcome, %1.", "Добро пожаловать, %1.", "Te damos la bienvenida, %1.", "欢迎，%1。", "أهلًا بك، ⁦%1⁩."}},
    {Copy::eDeclined, {"Your request was declined.", "Заявка отклонена.", "Tu solicitud fue rechazada.", "你的申请被拒绝了。", "رُفض طلبك."}},
    {Copy::eExcluded, {"You were excluded from the group.", "Вас исключили из группы.", "Te excluyeron del grupo.", "你已被移出群组。", "تم استبعادك من المجموعة."}},
    {Copy::eBlocked, {"You were blocked in the group.", "Вас заблокировали в группе.", "Te bloquearon en el grupo.", "你已被群组屏蔽。", "حُظرت في المجموعة."}},
    {Copy::eWantsToJoin, {"%1 wants to join.\n%2", "%1 хочет вступить.\n%2", "%1 quiere unirse.\n%2", "%1 想要加入。\n%2", "⁦%1⁩ يريد الانضمام.\n⁦%2⁩"}},
    {Copy::eAllowedBy, {"%1: allowed by %2.", "%1: одобрено админом %2.", "%1: aceptado por %2.", "%1：已由 %2 批准。", "⁦%1⁩: وافق عليه ⁦%2⁩."}},
    {Copy::eDeclinedBy, {"%1: declined by %2.", "%1: отклонено админом %2.", "%1: rechazado por %2.", "%1：已由 %2 拒绝。", "⁦%1⁩: رفضه ⁦%2⁩."}},
    {Copy::eAllowed, {"%1: allowed.", "%1: одобрено.", "%1: aceptado.", "%1：已批准。", "⁦%1⁩: تمت الموافقة."}},
    {Copy::eDeclinedCard, {"%1: declined.", "%1: отклонено.", "%1: rechazado.", "%1：已拒绝。", "⁦%1⁩: تم الرفض."}},
    {Copy::eAsksYou, {"%1 asks you to write.\n%2", "%1 просит написать лично.\n%2", "%1 te pide que le escribas.\n%2", "%1 请你私下写信。\n%2", "⁦%1⁩ يطلب أن تراسله مباشرة.\n⁦%2⁩"}},
    {Copy::eContactSent, {"Sent.", "Отправлено.", "Enviado.", "已发送。", "أُرسل."}},
    {Copy::eAdminGranted, {"You are an admin now.", "Теперь вы админ.", "Ahora tienes rol de administrador.", "你现在是管理员。", "أصبحت مشرفًا."}},
    {Copy::eAdminRevoked, {"You are no longer an admin.", "Вы больше не админ.", "Ya no tienes rol de administrador.", "你不再是管理员。", "لم تعد مشرفًا."}},
    {Copy::ePostingClosed, {"Only admins post here.", "Писать здесь могут только админы.", "Aquí solo publican los administradores.", "这里只有管理员可以发言。", "النشر هنا للمشرفين فقط."}},
    {Copy::eTooLarge, {"Too large to relay.", "Слишком большое для пересылки.", "Demasiado grande para reenviarlo.", "内容太大，无法转发。", "أكبر من أن يُعاد إرساله."}},
    {Copy::eButtonAllow, {"Allow", "Разрешить", "Permitir", "允许", "سماح"}},
    {Copy::eButtonDecline, {"Decline", "Отклонить", "Rechazar", "拒绝", "رفض"}},
    {Copy::eButtonContactOn, {"Accept contact requests", "Принимать запросы контакта", "Aceptar solicitudes de contacto", "接受联系请求", "قبول طلبات التواصل"}},
    {Copy::eButtonContactOff, {"Refuse contact requests", "Не принимать запросы контакта", "Rechazar solicitudes de contacto", "拒绝联系请求", "رفض طلبات التواصل"}},
    {Copy::eButtonSettings, {"Settings", "Настройки", "Ajustes", "设置", "الإعدادات"}},
    {Copy::eSettingsHint, {"Bring this up again with !!/settings!!.", "Вызвать это окно можно командой !!/settings!!.", "Vuelve a abrir esto con !!/settings!!.", "用 !!/settings!! 可再次打开这里。", "افتح هذا مجددًا بالأمر ⁦!!/settings!!⁩."}},
    {Copy::eButtonRename, {"Change name", "Сменить имя", "Cambiar nombre", "更改名字", "تغيير الاسم"}},
    {Copy::eAskNewName, {"Send your new name.", "Пришлите новое имя.", "Envía tu nuevo nombre.", "请发送你的新名字。", "أرسل اسمك الجديد."}},
    {Copy::eRenamed, {"Your name is now %1.", "Теперь вас зовут %1.", "Ahora te llamas %1.", "你现在的名字是 %1。", "اسمك الآن ⁦%1⁩."}},
};

std::size_t languageIndex(const std::string& language)
{
    for (std::size_t index = 0; index < kLanguageCount; ++index) {
        if (language == kGroupLanguages[index]) {
            return index;
        }
    }
    throw std::runtime_error("the group language is one of en, ru, es, zh, ar");
}

}  // namespace

std::string copyText(
    const std::string& language, const Copy key, const std::vector<std::string>& values)
{
    const std::size_t index = languageIndex(language);
    for (const Entry& entry : kEntries) {
        if (entry.key != key) {
            continue;
        }
        std::string text = entry.texts[index];
        for (std::size_t value = 0; value < values.size(); ++value) {
            const std::string mark = "%" + std::to_string(value + 1);
            for (std::size_t at = text.find(mark); at != std::string::npos;
                 at = text.find(mark, at + values[value].size())) {
                text.replace(at, mark.size(), values[value]);
            }
        }
        return text;
    }
    throw std::runtime_error("group copy: no text for a key");
}

bool copyComplete()
{
    for (const Entry& entry : kEntries) {
        for (const char* const text : entry.texts) {
            if (text == nullptr || *text == '\0') {
                return false;
            }
        }
    }
    return std::size(kEntries) == static_cast<std::size_t>(Copy::eBlocked) + 1;
}

}  // namespace bazarish::bot
