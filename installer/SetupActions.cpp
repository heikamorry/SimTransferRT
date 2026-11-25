#include <windows.h>
#include <msiquery.h>
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void checked(UINT code) {
    if (code != ERROR_SUCCESS)
        throw std::runtime_error("Windows Installer API failed");
}

template <class Reader> std::wstring readString(Reader reader) {
    wchar_t first = L'\0';
    DWORD length = 0;
    const auto code = reader(&first, &length);
    if (code == ERROR_SUCCESS)
        return {};
    if (code != ERROR_MORE_DATA)
        checked(code);
    std::wstring value(length + 1, L'\0');
    ++length;
    checked(reader(value.data(), &length));
    value.resize(length);
    return value;
}

std::wstring property(MSIHANDLE install, const wchar_t *key) {
    return readString([&](wchar_t *text, DWORD *size) {
        return MsiGetPropertyW(install, key, text, size);
    });
}

std::wstring field(MSIHANDLE record, UINT index) {
    return readString([&](wchar_t *text, DWORD *size) {
        return MsiRecordGetStringW(record, index, text, size);
    });
}

fs::path directory(MSIHANDLE install, const std::wstring &id) {
    return readString([&](wchar_t *text, DWORD *size) {
        return MsiGetTargetPathW(install, id.c_str(), text, size);
    });
}

std::wstring locationError(MSIHANDLE install) {
    const auto folder = directory(install, L"INSTALLFOLDER");
    if (!folder.is_absolute() || folder == folder.root_path() ||
        GetDriveTypeW(folder.root_path().c_str()) != DRIVE_FIXED)
        return L"请选择本地固定磁盘上的专用子目录。";
    PMSIHANDLE database = MsiGetActiveDatabase(install);
    PMSIHANDLE view;
    checked(MsiDatabaseOpenViewW(database,
                                 L"SELECT `FileName`, `Directory_` FROM `File`, "
                                 L"`Component` WHERE `File`.`Component_` = `Component`.`Component`",
                                 &view));
    checked(MsiViewExecute(view, 0));
    for (;;) {
        PMSIHANDLE row;
        const auto fetched = MsiViewFetch(view, &row);
        if (fetched == ERROR_NO_MORE_ITEMS)
            break;
        checked(fetched);
        auto name = field(row, 1);
        const auto separator = name.find(L'|');
        if (separator != std::wstring::npos)
            name.erase(0, separator + 1);
        const auto path = directory(install, field(row, 2)) / name;
        if (fs::exists(path))
            return L"目录中已有同名程序或组件（" + name + L"）。请选择其他目录，以保留原有文件。";
    }
    if (fs::exists(folder / L"SimTransfer.exe"))
        return L"此处已有手动部署的程序。请选择其他安装目录。";
    return {};
}

UINT inspectLocation(MSIHANDLE install, bool enforce) noexcept {
    try {
        const auto error = locationError(install);
        checked(MsiSetPropertyW(install, L"SimLocationError", error.c_str()));
        if (error.empty() || !enforce)
            return ERROR_SUCCESS;
        PMSIHANDLE message = MsiCreateRecord(0);
        checked(MsiRecordSetStringW(message, 0, error.c_str()));
        MsiProcessMessage(install, INSTALLMESSAGE_ERROR, message);
    } catch (...) {
        return ERROR_INSTALL_FAILURE;
    }
    return ERROR_INSTALL_FAILURE;
}

// Use Explorer's known folder in the interactive session. The token is queried
// only to identify the shortcut owner and resolve a redirected AppData folder.
void interactiveQuickLaunch(MSIHANDLE install) {
    DWORD buildSize = 32 * sizeof(wchar_t);
    wchar_t build[32]{};
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, build,
                     &buildSize) != ERROR_SUCCESS)
        return;
    const auto number = std::wcstol(build, nullptr, 10);
    if (number < 17763 || number >= 22000)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &pid);
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle shell(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), CloseHandle);
    HANDLE rawToken = nullptr;
    if (!shell || !OpenProcessToken(shell.get(), TOKEN_QUERY, &rawToken))
        return;
    Handle token(rawToken, CloseHandle);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!size || !GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
        return;
    wchar_t *sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid))
        return;
    std::unique_ptr<void, decltype(&LocalFree)> sidOwner(sid, LocalFree);
    const auto previous = property(install, L"SAVEDQUICKOWNER");
    if (!previous.empty() && previous != sid)
        return;
    wchar_t *path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_QuickLaunch, KF_FLAG_DONT_VERIFY, token.get(), &path)))
        return;
    std::unique_ptr<void, decltype(&CoTaskMemFree)> pathOwner(path, CoTaskMemFree);
    if (property(install, L"SAVEDQUICKDIR").empty())
        checked(MsiSetPropertyW(install, L"SIMQUICKDIR", path));
    checked(MsiSetPropertyW(install, L"SIMQUICKOWNER", sid));
    checked(MsiSetPropertyW(install, L"SIMQUICKAVAILABLE", L"1"));
}
} // namespace

extern "C" __declspec(dllexport) UINT __stdcall LocateQuickLaunch(MSIHANDLE install) noexcept {
    try {
        checked(MsiSetPropertyW(install, L"SIMQUICKAVAILABLE", L""));
        const auto saved = property(install, L"SAVEDQUICKDIR");
        if (!saved.empty()) {
            checked(MsiSetPropertyW(install, L"SIMQUICKDIR", saved.c_str()));
            const auto owner = property(install, L"SAVEDQUICKOWNER");
            checked(MsiSetPropertyW(install, L"SIMQUICKOWNER", owner.c_str()));
        }
        interactiveQuickLaunch(install);
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_INSTALL_FAILURE;
    }
}

extern "C" __declspec(dllexport) UINT __stdcall ReadFeatureChoices(MSIHANDLE install) noexcept {
    struct Choice {
        const wchar_t *feature;
        const wchar_t *property;
    };

    constexpr Choice choices[]{
        {L"CommandLine", L"SIMCLI"}, {L"Desktop", L"SIMDESKTOP"}, {L"QuickLaunch", L"SIMQUICK"}};
    for (const auto &choice : choices) {
        INSTALLSTATE existing = INSTALLSTATE_UNKNOWN;
        INSTALLSTATE planned = INSTALLSTATE_UNKNOWN;
        if (MsiGetFeatureStateW(install, choice.feature, &existing, &planned) != ERROR_SUCCESS)
            return ERROR_INSTALL_FAILURE;
        const auto state =
            planned == INSTALLSTATE_UNKNOWN || planned == INSTALLSTATE_DEFAULT ? existing : planned;
        if (MsiSetPropertyW(install, choice.property, state == INSTALLSTATE_LOCAL ? L"1" : L"") !=
            ERROR_SUCCESS)
            return ERROR_INSTALL_FAILURE;
    }
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall InspectNewLocation(MSIHANDLE install) noexcept {
    return inspectLocation(install, false);
}

extern "C" __declspec(dllexport) UINT __stdcall CheckNewLocation(MSIHANDLE install) noexcept {
    return inspectLocation(install, true);
}
