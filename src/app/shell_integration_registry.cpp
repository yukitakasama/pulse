#include "shell_integration_registry.h"

#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <vector>

namespace pulse::app {
namespace {

constexpr wchar_t kBackupRoot[] = L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\";
constexpr DWORD kSnapshotMagic = 0x31534950; // PIS1; one atomic REG_BINARY value per group.

struct Value {
    bool known = true;
    bool exists = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const Value&) const = default;
};
struct Binding { std::wstring key; std::wstring name; Value desired; };
struct Snapshot { std::vector<Value> before; std::vector<Value> written; };

bool SameValue(const Value& a, const Value& b) {
    return a.exists == b.exists && (!a.exists || (a.type == b.type && a.bytes == b.bytes));
}

Value String(const std::wstring& text) {
    Value value;
    value.exists = true;
    value.type = REG_SZ;
    value.bytes.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.bytes.data(), text.c_str(), value.bytes.size());
    return value;
}

std::wstring Text(const Value& value) {
    if (!value.exists || (value.type != REG_SZ && value.type != REG_EXPAND_SZ) ||
        value.bytes.size() % sizeof(wchar_t)) return {};
    std::wstring text(value.bytes.size() / sizeof(wchar_t), L'\0');
    if (!text.empty()) std::memcpy(text.data(), value.bytes.data(), value.bytes.size());
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}

bool Read(const std::wstring& key, const std::wstring& name, Value& value) {
    value = {};
    HKEY handle = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) return false;
    DWORD size = 0;
    status = RegQueryValueExW(handle, name.c_str(), nullptr, &value.type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) { RegCloseKey(handle); value = {}; return true; }
    if (status != ERROR_SUCCESS || size > 8 * 1024 * 1024) { RegCloseKey(handle); return false; }
    value.bytes.resize(size);
    status = RegQueryValueExW(handle, name.c_str(), nullptr, &value.type,
                             value.bytes.empty() ? nullptr : value.bytes.data(), &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) return false;
    value.bytes.resize(size);
    value.exists = true;
    return true;
}

bool Write(const std::wstring& key, const std::wstring& name, const Value& value) {
    HKEY handle = nullptr;
    LONG status = value.exists
        ? RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle);
    if (!value.exists && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)) return true;
    if (status != ERROR_SUCCESS) return false;
    status = value.exists
        ? RegSetValueExW(handle, name.c_str(), 0, value.type, value.bytes.empty() ? nullptr : value.bytes.data(),
                         static_cast<DWORD>(value.bytes.size()))
        : RegDeleteValueW(handle, name.c_str());
    RegCloseKey(handle);
    if (!value.exists && status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    Value actual;
    return status == ERROR_SUCCESS && Read(key, name, actual) && SameValue(actual, value);
}

void Put(std::vector<BYTE>& bytes, DWORD number) {
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(number));
    std::memcpy(bytes.data() + offset, &number, sizeof(number));
}
bool Take(const std::vector<BYTE>& bytes, size_t& offset, DWORD& number) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(number)) return false;
    std::memcpy(&number, bytes.data() + offset, sizeof(number));
    offset += sizeof(number);
    return true;
}
Value Encode(const Snapshot& snapshot) {
    Value encoded;
    encoded.exists = true;
    encoded.type = REG_BINARY;
    Put(encoded.bytes, kSnapshotMagic);
    Put(encoded.bytes, static_cast<DWORD>(snapshot.before.size()));
    for (size_t i = 0; i < snapshot.before.size(); ++i) {
        for (const Value* value : {&snapshot.before[i], &snapshot.written[i]}) {
            Put(encoded.bytes, (value->known ? 2u : 0u) | (value->exists ? 1u : 0u));
            Put(encoded.bytes, value->type);
            Put(encoded.bytes, static_cast<DWORD>(value->bytes.size()));
            encoded.bytes.insert(encoded.bytes.end(), value->bytes.begin(), value->bytes.end());
        }
    }
    return encoded;
}
bool Decode(const Value& encoded, size_t count, Snapshot& snapshot) {
    if (!encoded.exists || encoded.type != REG_BINARY) return false;
    size_t offset = 0;
    DWORD magic = 0, stored_count = 0;
    if (!Take(encoded.bytes, offset, magic) || magic != kSnapshotMagic ||
        !Take(encoded.bytes, offset, stored_count) || stored_count != count) return false;
    snapshot.before.resize(count);
    snapshot.written.resize(count);
    for (size_t i = 0; i < count; ++i) {
        for (Value* value : {&snapshot.before[i], &snapshot.written[i]}) {
            DWORD flags = 0, size = 0;
            if (!Take(encoded.bytes, offset, flags) || flags > 3 ||
                !Take(encoded.bytes, offset, value->type) || !Take(encoded.bytes, offset, size) ||
                size > encoded.bytes.size() - offset) return false;
            value->known = (flags & 2) != 0;
            value->exists = (flags & 1) != 0;
            value->bytes.assign(encoded.bytes.begin() + offset, encoded.bytes.begin() + offset + size);
            offset += size;
        }
    }
    return offset == encoded.bytes.size();
}

std::vector<Binding> Bindings(const std::wstring& group, const std::wstring& exe) {
    std::wstring shell, verb, line;
    if (group == L"WinE") {
        shell = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell";
        verb = shell + L"\\opennewwindow";
        line = L"\"" + exe + L"\"";
    } else if (group == L"ThisPc") {
        shell = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
        verb = shell + L"\\open";
        line = L"\"" + exe + L"\" \"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\"";
    } else {
        shell = L"Software\\Classes\\" + group + L"\\shell";
        verb = shell + L"\\open";
        line = L"\"" + exe + L"\" \"%1\"";
    }
    std::vector<Binding> result{{verb + L"\\command", L"", String(line)},
                                {verb + L"\\command", L"DelegateExecute", String(L"")}};
    if (group != L"WinE") result.push_back({shell, L"", String(L"open")});
    if (group == L"Directory" || group == L"Drive")
        result.push_back({verb, L"DelegateExecute", String(L"")}); // compatibility with old Pulse values
    return result;
}

std::vector<std::wstring> Groups(ShellIntegrationKind kind) {
    if (kind == ShellIntegrationKind::Folders) return {L"Directory", L"Drive"};
    if (kind == ShellIntegrationKind::Directory) return {L"Directory"};
    if (kind == ShellIntegrationKind::Drive) return {L"Drive"};
    return {kind == ShellIntegrationKind::WinE ? L"WinE" : L"ThisPc"};
}

void ImportLegacyBackup(const std::vector<Binding>& bindings, size_t i, Value& before) {
    const wchar_t* name = i == 1 ? L"PulseBackupDelegateExecute" : L"PulseBackup";
    Value backup;
    if (i > 2 || !Read(bindings[i].key, name, backup) || !backup.exists ||
        (backup.type != REG_SZ && backup.type != REG_EXPAND_SZ)) return;
    // Old versions retained text but not the original registry type. Recover
    // that text, while retaining the incomplete marker for honest reporting.
    before = backup;
    before.known = false;
}

void DropEmptyKeys(std::wstring key) {
    while (key.size() > std::wstring_view(L"Software\\Classes").size()) {
        if (SHDeleteEmptyKeyW(HKEY_CURRENT_USER, key.c_str()) != ERROR_SUCCESS) break;
        key.resize(key.rfind(L'\\'));
    }
}

bool LegacyResidue(std::vector<Binding>& bindings, std::vector<Value>& current) {
    bindings.clear();
    current.clear();
    for (const auto* group : {L"Directory", L"Drive", L"WinE", L"ThisPc"}) {
        const auto group_bindings = Bindings(group, L"");
        HKEY backup = nullptr;
        const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kBackupRoot) + group).c_str(),
                                         0, KEY_QUERY_VALUE, &backup);
        if (backup) RegCloseKey(backup);
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return false;
        for (size_t i = 0; i < group_bindings.size(); ++i) {
            const auto& binding = group_bindings[i];
            Value actual, legacy;
            if (!Read(binding.key, binding.name, actual)) return false;
            const bool absent = i == 0 || (i == 1 && (group == std::wstring_view(L"Directory") ||
                                                                     group == std::wstring_view(L"Drive")));
            if (!SameValue(actual, absent ? Value{} : binding.desired)) return false;
            for (const auto* name : {L"PulseBackup", L"PulseBackupDelegateExecute"})
                if (!Read(binding.key, name, legacy) || legacy.exists) return false;
            bindings.push_back(binding);
            current.push_back(std::move(actual));
        }
        // The legacy namespace shape has no delegate on the verb itself.
        if (group == std::wstring_view(L"WinE") || group == std::wstring_view(L"ThisPc")) {
            auto verb = group_bindings.front().key;
            verb.resize(verb.rfind(L'\\'));
            Value value;
            for (const auto* name : {L"DelegateExecute", L"PulseBackup", L"PulseBackupDelegateExecute"})
                if (!Read(verb, name, value) || value.exists) return false;
            if (group == std::wstring_view(L"WinE")) {
                verb.resize(verb.rfind(L'\\'));
                for (const auto* name : {L"", L"PulseBackup", L"PulseBackupDelegateExecute"})
                    if (!Read(verb, name, value) || value.exists) return false;
            }
        }
    }
    return true;
}

bool ApplyGroup(const std::wstring& group, const std::wstring& exe, bool on) {
    const auto bindings = Bindings(group, exe);
    std::vector<Value> current(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i)
        if (!Read(bindings[i].key, bindings[i].name, current[i])) return false;
    const std::wstring backup_key = std::wstring(kBackupRoot) + group;
    Value stored;
    if (!Read(backup_key, L"Snapshot", stored)) return false;
    Snapshot previous;
    const bool valid = Decode(stored, bindings.size(), previous);
    const bool command_ours = ShellCommandTargetsExecutable(Text(current[0]), exe);
    const bool owned = valid && command_ours && SameValue(current[0], previous.written[0]);
    // Older restores removed the command before clearing the other overrides.
    // Only a matching snapshot can prove ownership once that anchor is absent.
    const bool removed_anchor = valid && !current[0].exists && !previous.before[0].exists &&
        ShellCommandTargetsExecutable(Text(previous.written[0]), exe);

    if (on) {
        if (stored.exists && !valid) return false;
        Snapshot next;
        next.before = current;
        for (size_t i = 0; i < bindings.size(); ++i) {
            next.written.push_back(bindings[i].desired);
            if ((owned || removed_anchor) &&
                (SameValue(current[i], previous.written[i]) || (removed_anchor && i == 0)))
                next.before[i] = previous.before[i];
            else if (!valid && command_ours && (i == 0 || SameValue(current[i], bindings[i].desired))) {
                next.before[i].known = false; // legacy state is not an original-value backup
                next.before[i].exists = false;
                next.before[i].bytes.clear();
                ImportLegacyBackup(bindings, i, next.before[i]);
            }
        }
        const Value encoded = Encode(next);
        if (!SameValue(encoded, stored) && !Write(backup_key, L"Snapshot", encoded)) return false;
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (SameValue(current[i], bindings[i].desired)) continue;
            Value checked;
            if (!Read(bindings[i].key, bindings[i].name, checked) || !SameValue(checked, current[i]) ||
                !Write(bindings[i].key, bindings[i].name, bindings[i].desired)) {
                bool rolled_back = true;
                for (size_t j = i + 1; j-- > 0;) {
                    if (!Read(bindings[j].key, bindings[j].name, checked)) rolled_back = false;
                    else if (SameValue(checked, bindings[j].desired))
                        rolled_back = Write(bindings[j].key, bindings[j].name, current[j]) && rolled_back;
                }
                if (rolled_back) Write(backup_key, L"Snapshot", stored);
                return false;
            }
        }
        return true;
    }

    if (!command_ours && !removed_anchor) return true; // no evidence that these values belong to Pulse
    if (!valid) {
        // Migrate available legacy text through the same ownership checks.
        previous.before.resize(bindings.size());
        for (size_t i = 0; i < bindings.size(); ++i) {
            previous.before[i].known = false;
            ImportLegacyBackup(bindings, i, previous.before[i]);
            previous.written.push_back(i == 0 ? current[i] : bindings[i].desired);
        }
    }
    if (valid && !owned && !removed_anchor) return false; // someone edited Pulse's command
    bool ok = valid;
    bool write_failed = false;
    for (size_t i = bindings.size(); i-- > 0;) {
        if (i == 0 && write_failed) break; // retain the anchor so a failed restore can be retried
        Value anchor;
        if (!Read(bindings[0].key, bindings[0].name, anchor) || !SameValue(anchor, current[0])) {
            ok = false;
            break;
        }
        Value actual;
        if (!Read(bindings[i].key, bindings[i].name, actual)) { ok = false; write_failed = true; continue; }
        if (!SameValue(actual, previous.written[i])) continue;
        if (!previous.before[i].known) {
            // Retaining our empty DelegateExecute after removing our command
            // disables Explorer's inherited handler (#81). Restore any legacy
            // backup, otherwise remove only values still matching our writes.
            if (!Write(bindings[i].key, bindings[i].name, previous.before[i])) write_failed = true;
            ok = false;
        } else if (!Write(bindings[i].key, bindings[i].name, previous.before[i])) {
            ok = false;
            write_failed = true;
        }
    }
    for (const auto& binding : bindings) DropEmptyKeys(binding.key);
    if (ok) {
        const LONG removed = RegDeleteTreeW(HKEY_CURRENT_USER, backup_key.c_str());
        ok = removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
    }
    return ok;
}

} // namespace

bool ShellCommandTargetsExecutable(const std::wstring& command, const std::wstring& exe) {
    size_t start = 0;
    while (start < command.size() && iswspace(command[start])) ++start;
    size_t end = start;
    if (start < command.size() && command[start] == L'"') {
        end = command.find(L'"', ++start);
        if (end == std::wstring::npos) return false;
    } else {
        while (end < command.size() && !iswspace(command[end])) ++end;
    }
    return !exe.empty() && end - start == exe.size() &&
        CompareStringOrdinal(command.data() + start, static_cast<int>(end - start),
                             exe.data(), static_cast<int>(exe.size()), TRUE) == CSTR_EQUAL;
}

bool ApplyShellIntegration(ShellIntegrationKind kind, const std::wstring& exe, bool on) {
    if (exe.empty()) return false;
    // Do not record damaged legacy overrides as the next "original" state.
    // The explicit repair action must release them before enabling again.
    if (on && HasLegacyShellIntegrationResidue()) return false;
    bool ok = true;
    for (const auto& group : Groups(kind)) ok = ApplyGroup(group, exe, on) && ok;
    return ok;
}

bool ReadShellIntegration(ShellIntegrationKind kind, const std::wstring& exe) {
    if (exe.empty()) return false;
    for (const auto& group : Groups(kind)) {
        for (const auto& binding : Bindings(group, exe)) {
            Value actual;
            if (!Read(binding.key, binding.name, actual) || !SameValue(actual, binding.desired)) return false;
        }
    }
    return true;
}

bool HasShellIntegrationOwnership(ShellIntegrationKind kind, const std::wstring& exe) {
    if (exe.empty()) return false;
    for (const auto& group : Groups(kind)) {
        const auto bindings = Bindings(group, exe);
        const auto& binding = bindings.front();
        Value actual;
        if (!Read(binding.key, binding.name, actual)) continue;
        if (ShellCommandTargetsExecutable(Text(actual), exe)) return true;
        if (actual.exists) continue;
        Value stored;
        Snapshot snapshot;
        if (!Read(std::wstring(kBackupRoot) + group, L"Snapshot", stored) ||
            !Decode(stored, bindings.size(), snapshot) || snapshot.before[0].exists ||
            !ShellCommandTargetsExecutable(Text(snapshot.written[0]), exe)) continue;
        // A previous restore may have removed the command first. Keep repair
        // available while a snapshot-proven override still masks Explorer.
        for (size_t i = 1; i < bindings.size(); ++i) {
            if (Read(bindings[i].key, bindings[i].name, actual) && actual.exists &&
                SameValue(actual, snapshot.written[i]) &&
                !SameValue(actual, snapshot.before[i])) return true;
        }
    }
    return false;
}

bool HasLegacyShellIntegrationResidue() {
    std::vector<Binding> bindings;
    std::vector<Value> current;
    return LegacyResidue(bindings, current);
}

bool RepairLegacyShellIntegrationResidue() {
    std::vector<Binding> bindings;
    std::vector<Value> current;
    if (!LegacyResidue(bindings, current)) return false;
    // Retain an exact backup before deleting anything. Never overwrite an
    // earlier repair record, including after an interrupted repair.
    const std::wstring backup_key = std::wstring(kBackupRoot) + L"LegacyOrphanRepair";
    const Value encoded = Encode(Snapshot{current, std::vector<Value>(current.size())});
    Value existing;
    if (!Read(backup_key, L"Snapshot", existing) ||
        (existing.exists && !SameValue(existing, encoded)) ||
        (!existing.exists && !Write(backup_key, L"Snapshot", encoded))) return false;
    std::vector<Binding> checked_bindings;
    std::vector<Value> checked;
    if (!LegacyResidue(checked_bindings, checked) || checked != current) return false;
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (!current[i].exists) continue;
        Value actual;
        if (!Read(bindings[i].key, bindings[i].name, actual) || !SameValue(actual, current[i]) ||
            !Write(bindings[i].key, bindings[i].name, Value{})) {
            // A failed repair must not strand a new partial pattern that no
            // longer satisfies the conservative full-signature check.
            for (size_t j = 0; j <= i; ++j) {
                if (current[j].exists && Read(bindings[j].key, bindings[j].name, actual) && !actual.exists)
                    Write(bindings[j].key, bindings[j].name, current[j]);
            }
            return false;
        }
    }
    for (const auto& binding : bindings) DropEmptyKeys(binding.key);
    return true;
}

bool PrepareShellIntegrationUpgrade(ShellIntegrationKind kind, const std::wstring& exe) {
    if (exe.empty()) return false;
    bool ok = true;
    for (const auto& group : Groups(kind)) {
        const auto bindings = Bindings(group, exe);
        std::vector<Value> current(bindings.size());
        bool readable = true;
        for (size_t i = 0; i < bindings.size(); ++i)
            readable = Read(bindings[i].key, bindings[i].name, current[i]) && readable;
        if (!readable) { ok = false; continue; }
        if (!ShellCommandTargetsExecutable(Text(current[0]), exe)) continue;
        const std::wstring key = std::wstring(kBackupRoot) + group;
        Value stored;
        if (!Read(key, L"Snapshot", stored)) { ok = false; continue; }
        Snapshot snapshot;
        const bool valid = Decode(stored, bindings.size(), snapshot);
        if (stored.exists && (!valid || !SameValue(current[0], snapshot.written[0]))) { ok = false; continue; }
        if (!valid) {
            snapshot.before = current;
            for (size_t i = 0; i < bindings.size(); ++i) {
                if (i == 0 || SameValue(current[i], bindings[i].desired)) {
                    snapshot.before[i] = {};
                    snapshot.before[i].known = false;
                    ImportLegacyBackup(bindings, i, snapshot.before[i]);
                }
                snapshot.written.push_back(i == 0 ? current[i] : bindings[i].desired);
            }
            if (!Write(key, L"Snapshot", Encode(snapshot))) { ok = false; continue; }
        } else {
            for (size_t i = 1; i < bindings.size(); ++i)
                if (!SameValue(current[i], snapshot.written[i])) snapshot.before[i] = current[i];
            const auto updated = Encode(snapshot);
            if (!SameValue(updated, stored) && !Write(key, L"Snapshot", updated)) { ok = false; continue; }
        }
        ok = Write(key, L"PendingUpgrade", String(exe)) && ok;
    }
    return ok;
}

bool UpgradeShellIntegration(ShellIntegrationKind kind, const std::wstring& previous_exe,
                             const std::wstring& exe) {
    if (previous_exe.empty() || exe.empty()) return false;
    bool ok = true;
    for (const auto& group : Groups(kind)) {
        const auto old = Bindings(group, previous_exe);
        const auto wanted = Bindings(group, exe);
        std::vector<Value> current(old.size());
        bool readable = true;
        for (size_t i = 0; i < old.size(); ++i)
            readable = Read(old[i].key, old[i].name, current[i]) && readable;
        if (!readable) { ok = false; continue; }
        const std::wstring backup_key = std::wstring(kBackupRoot) + group;
        Value stored;
        if (!Read(backup_key, L"Snapshot", stored)) { ok = false; continue; }
        Snapshot snapshot;
        const bool valid = Decode(stored, old.size(), snapshot);
        if (stored.exists && !valid) { ok = false; continue; }
        Value pending;
        if (!Read(backup_key, L"PendingUpgrade", pending)) { ok = false; continue; }
        const bool restored_by_uninstaller = valid && Text(pending) == previous_exe &&
            SameValue(current[0], snapshot.before[0]);
        if (current[0].exists && !ShellCommandTargetsExecutable(Text(current[0]), previous_exe) &&
            !restored_by_uninstaller) continue;
        if (valid && (!ShellCommandTargetsExecutable(Text(snapshot.written[0]), previous_exe) ||
            (current[0].exists && !SameValue(current[0], snapshot.written[0]) && !restored_by_uninstaller))) {
            ok = false;
            continue;
        }
        if (!valid) {
            snapshot.before = current;
            for (size_t i = 0; i < old.size(); ++i) {
                // A pre-snapshot uninstaller may already have erased these.
                if (!current[i].exists || i == 0 || SameValue(current[i], old[i].desired)) {
                    snapshot.before[i].known = false;
                    snapshot.before[i].exists = false;
                    snapshot.before[i].bytes.clear();
                    ImportLegacyBackup(old, i, snapshot.before[i]);
                }
                snapshot.written.push_back(old[i].desired);
            }
        }
        const auto previous_written = snapshot.written;
        for (size_t i = 0; i < wanted.size(); ++i) snapshot.written[i] = wanted[i].desired;
        if (!Write(backup_key, L"Snapshot", Encode(snapshot))) { ok = false; continue; }
        for (size_t i = 0; i < wanted.size(); ++i) {
            if (current[i].exists && !SameValue(current[i], previous_written[i]) &&
                !(restored_by_uninstaller && SameValue(current[i], snapshot.before[i]))) continue;
            Value checked;
            if (!Read(wanted[i].key, wanted[i].name, checked) || !SameValue(checked, current[i]) ||
                !Write(wanted[i].key, wanted[i].name, wanted[i].desired)) {
                bool rolled_back = true;
                for (size_t j = i + 1; j-- > 0;) {
                    if (!Read(wanted[j].key, wanted[j].name, checked)) rolled_back = false;
                    else if (SameValue(checked, wanted[j].desired))
                        rolled_back = Write(wanted[j].key, wanted[j].name, current[j]) && rolled_back;
                }
                if (rolled_back) Write(backup_key, L"Snapshot", stored);
                ok = false;
                break;
            }
        }
        if (ok) Write(backup_key, L"PendingUpgrade", Value{});
    }
    return ok;
}

} // namespace pulse::app
