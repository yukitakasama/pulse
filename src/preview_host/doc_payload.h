// doc_payload.h — building the Markdown payload (markdown_document.h,
// PULSEMD) from other document formats: DOCX, EPUB, Jupyter notebooks.
//
// Records added for converted documents (all optional):
//   V \t chapters \t loaded \t total  integrity counts, not notebook metadata
//   M \t format \t a \t b     document facts for the status pill:
//                             docx: a = word count;  epub: a = title, b = author
//   P \t title                a section (EPUB chapter) starts; the reader shows
//                             one section at a time
//   C \t level \t section \t block \t title
//                             table of contents entry: section index and the
//                             block (within that section) it points at
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::preview {

enum : unsigned {
    kDocBold = 1, kDocItalic = 2, kDocCode = 4, kDocStrike = 8, kDocLink = 16, kDocImage = 32,
    kDocUnderline = 64,
};

void AppendPayloadField(std::wstring& out, std::wstring_view text);  // escapes \ tab newline

class DocPayload {
public:
    explicit DocPayload(size_t limit);

    bool full() const noexcept { return full_ || text_truncated_; }
    size_t blocks() const noexcept { return blocks_; }  // B records written
    std::wstring& str() noexcept { return out_; }

    // A whole record line (M, P, C...), dropped once the payload is full.
    void Record(const std::wstring& line);

    // Leaf block: Begin, any number of Text runs, End. End drops blocks with
    // no text unless keep_empty (table cells).
    void Begin(wchar_t kind, std::wstring arg = {}, int quote = 0, int indent = 0,
               std::wstring marker = {});
    void Text(std::wstring_view text, unsigned flags = 0, const std::wstring& target = {});
    bool open() const noexcept { return open_; }
    const std::wstring& text() const noexcept { return text_; }
    void End(bool keep_empty = false);

    void Image(const std::wstring& path, const std::wstring& alt, int quote = 0, int indent = 0);
    void Rule();
    // Tables: every row must get exactly `columns` cells (Begin('t') ... End(true)).
    void TableBegin(size_t columns, int quote = 0, int indent = 0);
    void TableRow();
    void TableEnd();

private:
    struct Run { size_t start, length; unsigned flags; std::wstring target; };
    std::wstring out_;
    size_t limit_;
    bool full_ = false;
    bool text_truncated_ = false;
    size_t blocks_ = 0;
    bool open_ = false;
    wchar_t kind_ = L'p';
    std::wstring arg_, marker_, text_;
    int quote_ = 0, indent_ = 0;
    std::vector<Run> runs_;
};

// Pictures from inside documents, written once under %TEMP%\Pulse\QuickLook
// (content-hashed names; files older than a day are pruned) so the reader can
// draw them through the thumbnail cache like any local picture.
class PreviewImageCache {
public:
    // extension with the dot (".png"); empty result on failure or past the
    // per-document budget.
    std::wstring Store(const std::vector<unsigned char>& bytes, std::wstring_view extension);
    std::wstring StoreBase64(const std::wstring& data, std::wstring_view extension);
    std::wstring StoreText(const std::wstring& text, std::wstring_view extension);  // as UTF-8

private:
    bool Ready();
    bool tried_ = false;
    std::wstring dir_;
    size_t stored_bytes_ = 0, stored_count_ = 0;
};

// Small tolerant pull parser for XML and XHTML parts. Names are local (prefix
// stripped); in html mode they are lower-cased, void elements close
// themselves and script/style content is skipped.
class XmlPull {
public:
    enum Kind { Eof, Open, Close, Text };
    XmlPull(std::wstring_view source, bool html) : s_(source), html_(html) {}
    Kind Next();
    const std::wstring& name() const noexcept { return name_; }
    const std::wstring& text() const noexcept { return text_; }  // Text: decoded
    bool self_closing() const noexcept { return self_closing_; }
    // Attribute by local name ("" when missing); qualified: exact "a:b" name.
    std::wstring Attr(std::wstring_view local) const;
    std::wstring QAttr(std::wstring_view qualified) const;
    // After Open: skips to the matching Close (nothing for self-closing).
    void SkipElement();

private:
    std::wstring_view s_;
    size_t pos_ = 0;
    bool html_;
    std::wstring name_, text_;
    bool self_closing_ = false;
    struct Attribute { std::wstring qualified, local, value; };
    std::vector<Attribute> attrs_;
};

std::wstring DecodeXmlEntities(std::wstring_view s);
// UTF-8 / UTF-16 (BOM) bytes to text.
std::wstring DecodeDocumentText(const std::vector<unsigned char>& bytes);
// ZIP member names: resolves "../" and "./" against base_dir (ending in '/'),
// percent-decodes, drops any #fragment; UTF-8 for ReadZipEntry.
std::wstring ResolvePartName(std::wstring_view base_dir, std::wstring_view href);
std::string ToUtf8(std::wstring_view s);

}  // namespace pulse::preview
