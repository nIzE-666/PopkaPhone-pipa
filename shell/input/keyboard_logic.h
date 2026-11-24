// Чистая логика состояния и маршрутизации экранной клавиатуры (без Qt — проверяется модульными тестами).
#pragma once

#include <string_view>

namespace naomi::keyboard {

inline std::string_view normalizeLayout(std::string_view layout)
{
    if (layout == "EN" || layout == "en" || layout == "En" || layout == "eN")
        return "EN";
    if (layout == "RU" || layout == "ru" || layout == "Ru" || layout == "rU")
        return "RU";
    return {};
}

inline std::string_view toggledLayout(std::string_view current)
{
    return normalizeLayout(current) == "RU" ? "EN" : "RU";
}

inline bool shouldShowOnAutoRequest(bool manualOnly, bool imEnabled)
{
    return !manualOnly && imEnabled;
}

inline bool shouldDismissOnFocusChange(bool visible, bool manual, bool hasFocus, bool imEnabled, bool dismissOnFocusLoss)
{
    if (!visible || manual)
        return false;
    return hasFocus ? !imEnabled : dismissOnFocusLoss;
}

inline bool shouldDismissOnImUpdate(bool visible, bool manual, bool hasFocus, bool imEnabled)
{
    return visible && !manual && hasFocus && !imEnabled;
}

inline bool shouldUseDirectInput(bool hasFocus, bool imEnabled, bool hasFallback, bool isWaylandItem)
{
    if (!hasFocus)
        return false;
    if (hasFallback && isWaylandItem)
        return false;
    return imEnabled;
}

} // namespace naomi::keyboard
