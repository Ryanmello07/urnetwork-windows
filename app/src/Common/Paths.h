// Per-process storage and log locations. The SDK owns persistence via
// NetworkSpaceManager(storagePath); each process gets its own dir, as on macOS
// (the app and the network extension do not share a container).
//
//   App (per user):   %LOCALAPPDATA%\URnetwork
//   Service (SYSTEM): %ProgramData%\URnetwork
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace urnw {

// Root storage dir for the current process, created if missing.
//   isService = true  -> %ProgramData%\URnetwork\service
//   isService = false -> %LOCALAPPDATA%\URnetwork\app
std::filesystem::path StorageRoot(bool isService);

// SDK storage path (passed to NetworkSpaceManager). Subdir of StorageRoot.
std::filesystem::path SdkStorageDir(bool isService);

// glog log directory (passed to urnw::setLogDir). Subdir of StorageRoot.
std::filesystem::path LogDir(bool isService);

// Persisted last-good RPC session file (app side only).
std::filesystem::path RpcSessionFile();

// The APP's own preferences (app side only).
//
// Distinct from the SDK LocalState, and it has to be: LocalState is a fixed set
// of typed accessors compiled into the SDK (getRouteLocal, getProvideControlMode,
// getBlockActionOverrides, ...) with no generic key/value pair anywhere in the C
// ABI, so a preference that belongs to THIS CLIENT rather than to the SDK has
// nowhere to live in it. Advanced Mode is the first of those. Same
// one-small-json-file idiom as rpc_session.json and in the same per-worktree
// StorageRoot, so two agents' worktrees do not share one preferences file.
std::filesystem::path AppPrefsFile();

// Read the app-preferences object (empty object when the file is missing or
// unreadable -- a preference is not worth taking the app down for), and write
// one key back with a whole-object read-modify-write. Never serialize just
// your own key: that deletes everyone else's. Promoted here at the third
// preference site (SdkHost, UpdateChecker, SubscriptionBalance), as the
// duplication note in those units prescribed.
//
// The read opens the file with every share flag, delete included, so it never
// blocks a save. A save writes the whole object to a uniquely named temp file
// beside app_prefs.json, flushes it to disk, and renames it over the file
// with POSIX semantics, which succeed while delete-sharing readers are open
// (MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) where the file system lacks
// them). The rename is retried briefly while a handle without delete-sharing
// (another process, an antivirus scan) blocks it. Saves in this process are
// serialized. A crash leaves the old file or the new one, never a truncated
// one. SaveAppPref returns false, and logs why, when the value did not reach
// the disk; the caller decides whether a session-only value is acceptable.
nlohmann::json LoadAppPrefs();
bool SaveAppPref(const char* key, const nlohmann::json& value);

// Type-checked reads of one preference from a LoadAppPrefs() object. A key
// that is missing, or holds another type, reads as the fallback (nullopt).
// nlohmann's value() substitutes its default only for a MISSING key and
// throws type_error.302 for a present one of the wrong type, so a hand-edited
// or older file used to throw on the startup path.
bool AppPrefBool(nlohmann::json const& prefs, std::string const& key,
                 bool fallback) noexcept;
std::optional<std::int64_t> AppPrefInt64(nlohmann::json const& prefs,
                                         std::string const& key) noexcept;

}  // namespace urnw
