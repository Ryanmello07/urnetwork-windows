// SPDX-License-Identifier: MPL-2.0
#include "InstallLocationWin32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace urnw::install {

static_assert(kWriteData == FILE_WRITE_DATA && kAppendData == FILE_APPEND_DATA &&
              kWriteEa == FILE_WRITE_EA && kDeleteChild == FILE_DELETE_CHILD &&
              kWriteAttributes == FILE_WRITE_ATTRIBUTES && kDelete == DELETE &&
              kWriteDac == WRITE_DAC && kWriteOwner == WRITE_OWNER);
static_assert(update::kAttributeDirectory == FILE_ATTRIBUTE_DIRECTORY &&
              update::kAttributeReparsePoint == FILE_ATTRIBUTE_REPARSE_POINT &&
              update::kAllowAceType == ACCESS_ALLOWED_ACE_TYPE &&
              update::kInheritedByFilesAndFolders == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
              update::kFileAllAccess == FILE_ALL_ACCESS &&
              update::kFileReadAndExecute == (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) &&
              update::kMarkerUsersAccess == (READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES));

namespace {

namespace fs = std::filesystem;

class Handle {
 public:
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return handle_; }
  bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_;
};

bool EqualsCaseless(std::wstring_view a, std::wstring_view b) {
  return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// `child` is a path strictly below `parent`.
bool IsBelow(std::wstring_view child, std::wstring_view parent) {
  return child.size() > parent.size() + 1 && child[parent.size()] == L'\\' &&
         EqualsCaseless(child.substr(0, parent.size()), parent);
}

// The path the file system resolves `path` to, every junction, symbolic link,
// mount point and substituted drive followed, as a drive-letter path; empty
// when it cannot be opened or resolves to a network path.
std::wstring FinalPath(const fs::path& path) {
  Handle handle(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.valid()) return {};
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = ::GetFinalPathNameByHandleW(handle.get(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()),
                                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    buffer.resize(length + 1);
  }
  constexpr std::wstring_view kUnc = L"\\\\?\\UNC\\";
  constexpr std::wstring_view kLocal = L"\\\\?\\";
  if (std::wstring_view(buffer).substr(0, kUnc.size()) == kUnc) return {};
  if (std::wstring_view(buffer).substr(0, kLocal.size()) == kLocal) buffer.erase(0, kLocal.size());
  return buffer;
}

// `path` absolute, with every short (8.3) component expanded; empty on failure.
std::wstring LongFullPath(const fs::path& path) {
  std::wstring full(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length == 0) return {};
    if (length < full.size()) {
      full.resize(length);
      break;
    }
    full.resize(length + 1);
  }
  std::wstring longPath(full.size() + 1, L'\0');
  for (;;) {
    const DWORD length = ::GetLongPathNameW(full.c_str(), longPath.data(),
                                            static_cast<DWORD>(longPath.size()));
    if (length == 0) return {};
    if (length < longPath.size()) {
      longPath.resize(length);
      break;
    }
    longPath.resize(length + 1);
  }
  while (longPath.size() > 3 && (longPath.back() == L'\\' || longPath.back() == L'/'))
    longPath.pop_back();
  return longPath;
}

std::wstring ProgramFilesPath() {
  PWSTR raw = nullptr;
  std::wstring path;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw)))
    path = raw;
  if (raw) ::CoTaskMemFree(raw);
  return path;
}

// The token whose rights the location is judged for, as an identification
// token for AccessCheck: this process's own token with Administrators made
// deny-only, or when this process is elevated, its linked (filtered) token.
// Either way it is what the user holds without consenting to elevation.
HANDLE NonAdminToken(std::string& error) {
  HANDLE raw = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) {
    error = std::format("OpenProcessToken failed: {}", ::GetLastError());
    return nullptr;
  }
  Handle process(raw);
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
  DWORD size = 0;
  if (!::GetTokenInformation(process.get(), TokenElevationType, &type, sizeof(type), &size)) {
    error = std::format("GetTokenInformation(TokenElevationType) failed: {}", ::GetLastError());
    return nullptr;
  }
  HANDLE source = process.get();
  HANDLE linkedToken = nullptr;
  if (type == TokenElevationTypeFull) {
    TOKEN_LINKED_TOKEN linked{};
    if (!::GetTokenInformation(process.get(), TokenLinkedToken, &linked, sizeof(linked), &size)) {
      error = std::format("GetTokenInformation(TokenLinkedToken) failed: {}", ::GetLastError());
      return nullptr;
    }
    linkedToken = linked.LinkedToken;
    source = linkedToken;
  }
  Handle linked(linkedToken);
  BYTE adminsSid[SECURITY_MAX_SID_SIZE];
  DWORD sidSize = sizeof(adminsSid);
  if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminsSid, &sidSize)) {
    error = std::format("CreateWellKnownSid failed: {}", ::GetLastError());
    return nullptr;
  }
  SID_AND_ATTRIBUTES disable{.Sid = adminsSid, .Attributes = 0};
  HANDLE restricted = nullptr;
  if (!::CreateRestrictedToken(source, 0, 1, &disable, 0, nullptr, 0, nullptr, &restricted)) {
    error = std::format("CreateRestrictedToken failed: {}", ::GetLastError());
    return nullptr;
  }
  Handle restrictedToken(restricted);
  HANDLE identification = nullptr;
  if (!::DuplicateToken(restrictedToken.get(), SecurityIdentification, &identification)) {
    error = std::format("DuplicateToken failed: {}", ::GetLastError());
    return nullptr;
  }
  return identification;
}

// The access `token` gets to `path` (MAXIMUM_ALLOWED), or false with `error`.
bool RightsFor(HANDLE token, const fs::path& path, std::uint32_t& rights, std::string& error) {
  constexpr SECURITY_INFORMATION kInfo =
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  DWORD needed = 0;
  ::GetFileSecurityW(path.c_str(), kInfo, nullptr, 0, &needed);
  if (needed == 0) {
    error = std::format("GetFileSecurity failed: {}", ::GetLastError());
    return false;
  }
  std::vector<BYTE> descriptor(needed);
  if (!::GetFileSecurityW(path.c_str(), kInfo, descriptor.data(), needed, &needed)) {
    error = std::format("GetFileSecurity failed: {}", ::GetLastError());
    return false;
  }
  GENERIC_MAPPING mapping{.GenericRead = FILE_GENERIC_READ,
                          .GenericWrite = FILE_GENERIC_WRITE,
                          .GenericExecute = FILE_GENERIC_EXECUTE,
                          .GenericAll = FILE_ALL_ACCESS};
  PRIVILEGE_SET privileges{};
  DWORD privilegesLength = sizeof(privileges);
  DWORD granted = 0;
  BOOL status = FALSE;
  if (!::AccessCheck(descriptor.data(), token, MAXIMUM_ALLOWED, &mapping, &privileges,
                     &privilegesLength, &granted, &status)) {
    error = std::format("AccessCheck failed: {}", ::GetLastError());
    return false;
  }
  // a refused MAXIMUM_ALLOWED grants nothing at all
  rights = status ? granted : 0;
  return true;
}

}  // namespace

bool QueryInstallLocation(const fs::path& folder, const fs::path& executable,
                          InstallPathInfo& path, InstallRights& rights, std::string& error) {
  path = {};
  rights = {};
  const std::wstring programFiles = ProgramFilesPath();
  const std::wstring programFilesFinal = programFiles.empty() ? L"" : FinalPath(programFiles);
  if (programFilesFinal.empty()) {
    error = "the Program Files folder could not be resolved";
    return false;
  }
  const std::wstring lexical = LongFullPath(folder);
  const std::wstring final = FinalPath(folder);
  if (lexical.empty() || final.empty()) {
    error = "the install folder could not be resolved";
    return false;
  }
  path.underProgramFiles = IsBelow(final, programFilesFinal);
  path.resolvesToItself = EqualsCaseless(lexical, final);
  // every folder from the install folder up to Program Files, which a
  // resolved path below it reaches
  for (fs::path at = lexical; !at.empty();) {
    const DWORD attributes = ::GetFileAttributesW(at.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      error = std::format("GetFileAttributes failed: {}", ::GetLastError());
      return false;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) path.reparsePoint = true;
    if (!IsBelow(at.native(), programFiles) || at.parent_path() == at) break;
    at = at.parent_path();
  }

  HANDLE raw = NonAdminToken(error);
  if (!raw) return false;
  Handle token(raw);
  return RightsFor(token.get(), folder, rights.folder, error) &&
         RightsFor(token.get(), executable, rights.executable, error);
}

fs::path OwnExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    if (length < path.size()) {
      path.resize(length);
      return fs::path(std::move(path));
    }
    if (path.size() >= 0x8000) return {};  // beyond the NT path limit
    path.resize(path.size() * 2);
  }
}

bool AdminOnlyPath(const fs::path& path, std::string& why) {
  HANDLE raw = NonAdminToken(why);
  if (!raw) return false;
  Handle token(raw);
  std::uint32_t rights = 0;
  if (!RightsFor(token.get(), path, rights, why)) return false;
  if ((rights & kWriteRights) == 0) return true;
  why = std::format("a token without admin rights gets 0x{:x} on it", rights);
  return false;
}

update::PathFacts ReadPathFacts(const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    const DWORD error = ::GetLastError();
    return {.found = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                         ? update::PathFacts::Found::Absent
                         : update::PathFacts::Found::Unreadable};
  }
  return {.found = update::PathFacts::Found::Present,
          .attributes = data.dwFileAttributes,
          .size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
}

namespace {

std::string SidText(PSID sid) {
  LPWSTR text = nullptr;
  if (!sid || !::ConvertSidToStringSidW(sid, &text)) return {};
  std::string ascii;
  for (const wchar_t* c = text; *c; ++c) ascii.push_back(static_cast<char>(*c));
  ::LocalFree(text);
  return ascii;
}

}  // namespace

update::DescriptorFacts ReadDescriptorFacts(const fs::path& path) {
  update::DescriptorFacts facts;
  constexpr SECURITY_INFORMATION kInfo = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  DWORD needed = 0;
  ::GetFileSecurityW(path.c_str(), kInfo, nullptr, 0, &needed);
  if (needed == 0) return facts;
  std::vector<BYTE> buffer(needed);
  const PSECURITY_DESCRIPTOR descriptor = buffer.data();
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  PSID owner = nullptr;
  BOOL ownerDefaulted = FALSE;
  BOOL present = FALSE;
  PACL dacl = nullptr;
  BOOL daclDefaulted = FALSE;
  if (!::GetFileSecurityW(path.c_str(), kInfo, descriptor, needed, &needed) ||
      !::GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !::GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) ||
      !::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &daclDefaulted)) {
    return facts;
  }
  facts.owner = SidText(owner);
  facts.daclPresent = present && dacl;
  facts.daclProtected = (control & SE_DACL_PROTECTED) != 0;
  if (facts.daclPresent) {
    ACL_SIZE_INFORMATION size{};
    if (!::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation)) return facts;
    for (DWORD i = 0; i < size.AceCount; ++i) {
      void* raw = nullptr;
      if (!::GetAce(dacl, i, &raw)) return facts;
      const auto* header = static_cast<const ACE_HEADER*>(raw);
      update::AceFacts ace{.type = header->AceType, .flags = header->AceFlags};
      // the allow and the deny ACE share one layout: a mask, then the SID
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE) {
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        ace.mask = allowed->Mask;
        ace.sid = SidText(const_cast<DWORD*>(&allowed->SidStart));
      }
      facts.aces.push_back(std::move(ace));
    }
  }
  facts.read = !facts.owner.empty();
  return facts;
}

fs::path ChannelMarkerPath(const fs::path& installFolder) {
  return installFolder / L"updates" / L"developer-channel";
}

update::ChannelMarker ReadChannelMarker(const fs::path& installFolder, std::string& why) {
  const fs::path updates = installFolder / L"updates";
  const fs::path marker = ChannelMarkerPath(installFolder);
  update::PathFacts folderFacts = ReadPathFacts(updates);
  update::DescriptorFacts folderDescriptor;
  update::PathFacts markerFacts;
  update::DescriptorFacts markerDescriptor;
  if (folderFacts.found == update::PathFacts::Found::Present) {
    folderDescriptor = ReadDescriptorFacts(updates);
    markerFacts = ReadPathFacts(marker);
    if (markerFacts.found == update::PathFacts::Found::Present) {
      markerDescriptor = ReadDescriptorFacts(marker);
    }
  }
  const update::MarkerJudgement judged =
      update::JudgeMarker(folderFacts, folderDescriptor, markerFacts, markerDescriptor);
  why.assign(judged.why);
  return judged.marker;
}

bool AdminOnlyLocation(const fs::path& executable, std::string& why) {
  InstallPathInfo path;
  InstallRights rights;
  if (!QueryInstallLocation(executable.parent_path(), executable, path, rights, why)) return false;
  if (IsAdminOnlyInstallDir(path, rights)) return true;
  why = std::format(
      "not an admin-only install location (below Program Files: {}, resolves to itself: {}, "
      "reparse point: {}, non-admin rights on the folder 0x{:x}, on the executable 0x{:x})",
      path.underProgramFiles, path.resolvesToItself, path.reparsePoint, rights.folder,
      rights.executable);
  return false;
}

}  // namespace urnw::install
