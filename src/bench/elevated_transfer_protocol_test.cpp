#include "../ipc/elevated_transfer_protocol.h"
#include <iostream>
int main() {
    using namespace pulse::elevated;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failed; };
    Nonce nonce{}; nonce[3] = 42;
    Header header; header.nonce = nonce;
    check(ValidHeader(header, nonce), "valid session frame");
    auto other = nonce; ++other[0];
    check(!ValidHeader(header, other), "wrong session nonce rejected");
    header.bytes = kMaxPayload + 1;
    check(!ValidHeader(header, nonce), "oversized frame rejected before allocation");
    header.bytes = 0; header.kind = static_cast<Kind>(99);
    check(!ValidHeader(header, nonce), "unknown opcode rejected");
    header.kind = Kind::RecycleDelete;
    check(ValidHeader(header, nonce) && Kind::RecycleDelete != Kind::PermanentDelete, "recycle and permanent requests have distinct bounded opcodes");
    header.version = kVersion - 1;
    check(!ValidHeader(header, nonce), "old protocol cannot accidentally dispatch new delete opcodes");
    Writer writer; writer.Number<uint32_t>(1); writer.Text(L"sample");
    Reader reader{writer.bytes};
    check(reader.Number<uint32_t>() == 1 && reader.Text() == L"sample" && reader.Done(), "bounded round trip");
    writer.bytes.pop_back(); Reader short_reader{writer.bytes}; short_reader.Number<uint32_t>(); short_reader.Text();
    check(!short_reader.Done(), "truncated UTF-16 rejected");
    Writer overflow; overflow.Text(std::wstring(kMaxString + 1, L'x'));
    check(!overflow.good, "oversized path rejected");
    Writer embedded; embedded.Text(std::wstring(L"a\0b", 3)); Reader embedded_reader{embedded.bytes}; embedded_reader.Text();
    check(!embedded_reader.Done(), "embedded null rejected");
    check(SafePath(L"C:\\folder\\file.txt") && SafePath(L"\\\\server\\share\\file.txt") && SafePath(L"\\\\?\\C:\\long\\file.txt"), "absolute filesystem paths accepted");
    check(!SafePath(L"relative.txt") && !SafePath(L"C:\\a\\..\\b") && !SafePath(L"\\\\.\\PhysicalDrive0") &&
          !SafePath(L"C:\\a:stream") && !SafePath(L"C:\\NUL.txt") && !SafePath(L"\\\\server\\pipe\\x"), "relative traversal device stream and pipe paths rejected");
    Nonce parsed{};
    check(ParseNonce(NonceText(nonce), parsed) && parsed == nonce && !ParseNonce(L"xyz", parsed), "nonce parser exact length and alphabet");
    return failed ? 1 : 0;
}
