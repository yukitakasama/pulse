#pragma once
#include <string_view>
#include <initializer_list>

namespace pulse::json {

// Syntax validation before permissive field extractors touch persisted state.
// Places historically wrote 0xRRGGBB; allow that extension only for that file.
class ConfigSyntax {
public:
    ConfigSyntax(std::wstring_view text, bool legacy_hex) : text_(text), hex_(legacy_hex) {}
    bool Object() { Space(); if (Peek() != L'{') return false; return Value(0) && (Space(), pos_ == text_.size()); }
    bool Array() { Space(); if (Peek() != L'[') return false; return Value(0) && (Space(), pos_ == text_.size()); }
private:
    wchar_t Peek() const { return pos_ < text_.size() ? text_[pos_] : L'\0'; }
    void Space() { while (Peek() == L' ' || Peek() == L'\r' || Peek() == L'\n' || Peek() == L'\t') ++pos_; }
    bool Eat(wchar_t c) { Space(); if (Peek() != c) return false; ++pos_; return true; }
    static bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
    static bool Hex(wchar_t c) { return Digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); }
    bool String() {
        if (!Eat(L'"')) return false;
        while (pos_ < text_.size()) {
            wchar_t c = text_[pos_++];
            if (c == L'"') return true;
            if (c < 0x20) return false;
            if (c != L'\\') continue;
            if (pos_ == text_.size()) return false;
            c = text_[pos_++];
            if (c == L'u') {
                for (int i = 0; i < 4; ++i) { if (!Hex(Peek())) return false; ++pos_; }
            } else if (c != L'"' && c != L'\\' && c != L'/' && c != L'b' &&
                       c != L'f' && c != L'n' && c != L'r' && c != L't') return false;
        }
        return false;
    }
    bool Value(unsigned depth) {
        if (depth > 64) return false;
        Space();
        if (Peek() == L'"') return String();
        if (Peek() == L'{' || Peek() == L'[') {
            const bool object = text_[pos_++] == L'{';
            const wchar_t close = object ? L'}' : L']';
            if (Eat(close)) return true;
            do {
                if (object && (!String() || !Eat(L':'))) return false;
                if (!Value(depth + 1)) return false;
                if (Eat(close)) return true;
            } while (Eat(L','));
            return false;
        }
        for (auto word : {std::wstring_view(L"true"), std::wstring_view(L"false"), std::wstring_view(L"null")}) {
            if (text_.substr(pos_, word.size()) == word) { pos_ += word.size(); return true; }
        }
        if (Peek() == L'-') ++pos_;
        if (!Digit(Peek())) return false;
        if (Peek() == L'0') {
            ++pos_;
            if (hex_ && (Peek() == L'x' || Peek() == L'X')) {
                ++pos_; if (!Hex(Peek())) return false;
                while (Hex(Peek())) ++pos_;
                return true;
            }
        } else while (Digit(Peek())) ++pos_;
        if (Peek() == L'.') { ++pos_; if (!Digit(Peek())) return false; while (Digit(Peek())) ++pos_; }
        if (Peek() == L'e' || Peek() == L'E') {
            ++pos_; if (Peek() == L'+' || Peek() == L'-') ++pos_;
            if (!Digit(Peek())) return false;
            while (Digit(Peek())) ++pos_;
        }
        return true;
    }
    std::wstring_view text_;
    size_t pos_ = 0;
    bool hex_;
};

inline bool ValidConfigObject(std::wstring_view text, bool legacy_hex = false) {
    return ConfigSyntax(text, legacy_hex).Object();
}
inline bool ValidConfigArray(std::wstring_view text) {
    return ConfigSyntax(text, false).Array();
}
} // namespace pulse::json
