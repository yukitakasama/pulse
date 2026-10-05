#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {

enum class ShellIntegrationKind { Folders, WinE, ThisPc, Directory, Drive };

// Uses HKCU (redirectable with RegOverridePredefKey in isolated tests).
// A false restore result can mean that legacy registrations had no exact
// backup: matching Pulse overrides are removed, unrelated values stay put.
bool ApplyShellIntegration(ShellIntegrationKind kind, const std::wstring& exe, bool on);
bool ReadShellIntegration(ShellIntegrationKind kind, const std::wstring& exe);
bool HasShellIntegrationOwnership(ShellIntegrationKind kind, const std::wstring& exe);
// Exact pre-snapshot orphan signature, used by startup migration and explicit
// repair. Ordinary enable/disable must not infer ownership of arbitrary values.
bool HasLegacyShellIntegrationResidue();
bool RepairLegacyShellIntegrationResidue();
bool PrepareShellIntegrationUpgrade(ShellIntegrationKind kind, const std::wstring& exe);
// Installer only: the selected group was owned immediately before the old
// uninstaller ran. Missing values may have been removed by that old uninstaller.
bool UpgradeShellIntegration(ShellIntegrationKind kind, const std::wstring& previous_exe,
                             const std::wstring& exe);
bool ShellCommandTargetsExecutable(const std::wstring& command, const std::wstring& exe);

} // namespace pulse::app
