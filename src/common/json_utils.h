#pragma once

#include <string>
#include <vector>

namespace pulse::json {

inline void Escape(const std::wstring& input, std::wstring& output) {
    for (wchar_t c : input) {
        if (c == L'\\') output += L"\\\\";
        else if (c == L'"') output += L"\\\"";
        else if (c == L'\n') output += L"\\n";
        else if (c == L'\r') output += L"\\r";
        else if (c == L'\t') output += L"\\t";
        else output += c;
    }
}

inline void SkipWhitespace(const std::wstring& input, size_t& pos) {
    while (pos < input.size() &&
           (input[pos] == L' ' || input[pos] == L'\n' ||
            input[pos] == L'\r' || input[pos] == L'\t')) {
        ++pos;
    }
}

inline size_t ValuePosition(const std::wstring& input, const std::wstring& key) {
    const std::wstring quoted = L"\"" + key + L"\"";
    size_t pos = input.find(quoted);
    if (pos == std::wstring::npos) return pos;
    pos = input.find(L':', pos + quoted.size());
    if (pos == std::wstring::npos) return pos;
    ++pos;
    SkipWhitespace(input, pos);
    return pos;
}

inline std::wstring UnescapeString(const std::wstring& input, size_t& pos) {
    std::wstring output;
    while (pos < input.size() && input[pos] != L'"') {
        if (input[pos] == L'\\' && pos + 1 < input.size()) {
            ++pos;
            if (input[pos] == L'n') output += L'\n';
            else if (input[pos] == L'r') output += L'\r';
            else if (input[pos] == L't') output += L'\t';
            else output += input[pos];
        } else {
            output += input[pos];
        }
        ++pos;
    }
    if (pos < input.size()) ++pos;
    return output;
}

inline std::wstring ExtractString(const std::wstring& input, const std::wstring& key,
                                  std::wstring fallback = {}) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'"')
        return fallback;
    ++pos;
    return UnescapeString(input, pos);
}

inline int ExtractInt(const std::wstring& input, const std::wstring& key, int fallback = 0) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size()) return fallback;
    int sign = 1;
    if (input[pos] == L'-') { sign = -1; ++pos; }
    if (pos >= input.size() || input[pos] < L'0' || input[pos] > L'9') return fallback;
    int value = 0;
    while (pos < input.size() && input[pos] >= L'0' && input[pos] <= L'9') {
        value = value * 10 + (input[pos] - L'0');
        ++pos;
    }
    return value * sign;
}

inline bool ExtractBool(const std::wstring& input, const std::wstring& key,
                        bool fallback = false) {
    const size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos) return fallback;
    if (input.compare(pos, 4, L"true") == 0) return true;
    if (input.compare(pos, 5, L"false") == 0) return false;
    return fallback;
}

inline std::vector<std::wstring> ExtractStringArray(const std::wstring& input,
                                                    const std::wstring& key) {
    std::vector<std::wstring> output;
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'[')
        return output;
    ++pos;
    while (pos < input.size()) {
        SkipWhitespace(input, pos);
        while (pos < input.size() && input[pos] == L',') {
            ++pos;
            SkipWhitespace(input, pos);
        }
        if (pos >= input.size() || input[pos] == L']') break;
        if (input[pos] != L'"') { ++pos; continue; }
        ++pos;
        output.push_back(UnescapeString(input, pos));
    }
    return output;
}

// Index of the bracket that closes the '[' or '{' at `open`, skipping string
// contents and nested values; npos when the input is unbalanced. Every reader
// of nested values goes through this instead of looking for the next ']'/'}'
// (paths and menu text may contain brackets and braces).
inline size_t MatchingClose(const std::wstring& input, size_t open) {
    if (open >= input.size() || (input[open] != L'[' && input[open] != L'{'))
        return std::wstring::npos;
    std::wstring closers;
    bool in_string = false;
    for (size_t i = open; i < input.size(); ++i) {
        const wchar_t c = input[i];
        if (in_string) {
            if (c == L'\\') ++i;
            else if (c == L'"') in_string = false;
            continue;
        }
        if (c == L'"') in_string = true;
        else if (c == L'[') closers.push_back(L']');
        else if (c == L'{') closers.push_back(L'}');
        else if (c == L']' || c == L'}') {
            if (closers.empty() || closers.back() != c) return std::wstring::npos;
            closers.pop_back();
            if (closers.empty()) return i;
        }
    }
    return std::wstring::npos;
}

// The raw text of key's array / object value, brackets included. Empty when
// the key is missing, holds another type, or the value is unbalanced.
inline std::wstring ExtractContainer(const std::wstring& input, const std::wstring& key,
                                     wchar_t open) {
    const size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != open) return {};
    const size_t close = MatchingClose(input, pos);
    return close == std::wstring::npos ? std::wstring{} : input.substr(pos, close - pos + 1);
}
inline std::wstring ExtractArray(const std::wstring& input, const std::wstring& key) {
    return ExtractContainer(input, key, L'[');
}
inline std::wstring ExtractObject(const std::wstring& input, const std::wstring& key) {
    return ExtractContainer(input, key, L'{');
}

// Reads the value at pos (a string with its quotes, an array / object, or a
// scalar) into raw and moves pos past it.
inline bool NextValue(const std::wstring& input, size_t& pos, std::wstring& raw) {
    SkipWhitespace(input, pos);
    if (pos >= input.size()) return false;
    const size_t start = pos;
    const wchar_t c = input[pos];
    if (c == L'"') {
        ++pos;
        while (pos < input.size() && input[pos] != L'"') pos += input[pos] == L'\\' ? 2 : 1;
        if (pos >= input.size()) return false;
        ++pos;
    } else if (c == L'[' || c == L'{') {
        const size_t close = MatchingClose(input, pos);
        if (close == std::wstring::npos) return false;
        pos = close + 1;
    } else {
        while (pos < input.size() && input[pos] != L',' && input[pos] != L'}' && input[pos] != L']' &&
               input[pos] != L' ' && input[pos] != L'\n' && input[pos] != L'\r' && input[pos] != L'\t')
            ++pos;
        if (pos == start) return false;
    }
    raw = input.substr(start, pos - start);
    return true;
}

// Calls fn(raw_element) for each element of an array ("[...]" text).
// False when the array is malformed (elements before the error were passed).
template <class Fn>
bool ForEachElement(const std::wstring& array, Fn&& fn) {
    if (array.size() < 2 || array.front() != L'[') return false;
    size_t pos = 1;
    for (;;) {
        SkipWhitespace(array, pos);
        if (pos >= array.size()) return false;
        if (array[pos] == L']') return true;
        std::wstring raw;
        if (!NextValue(array, pos, raw)) return false;
        fn(raw);
        SkipWhitespace(array, pos);
        if (pos < array.size() && array[pos] == L',') ++pos;
    }
}

// Calls fn(key, raw_value) for each member of an object ("{...}" text).
template <class Fn>
bool ForEachMember(const std::wstring& object, Fn&& fn) {
    if (object.size() < 2 || object.front() != L'{') return false;
    size_t pos = 1;
    for (;;) {
        SkipWhitespace(object, pos);
        if (pos >= object.size()) return false;
        if (object[pos] == L'}') return true;
        if (object[pos] != L'"') return false;
        ++pos;
        const std::wstring key = UnescapeString(object, pos);
        SkipWhitespace(object, pos);
        if (pos >= object.size() || object[pos] != L':') return false;
        ++pos;
        std::wstring raw;
        if (!NextValue(object, pos, raw)) return false;
        fn(key, raw);
        SkipWhitespace(object, pos);
        if (pos < object.size() && object[pos] == L',') ++pos;
    }
}

} // namespace pulse::json
