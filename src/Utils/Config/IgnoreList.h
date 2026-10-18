#pragma once

#include "Steam/Types.h"

#include <string>
#include <vector>

// IgnoreList — reads ignore.txt from each Lua config directory.
//
// Any appid listed in ignore.txt (one per line, decimal, comments with #)
// is treated as "update suppressed": BuildDepotDependency will pin its
// manifest gid and HandleSend for GetManifestRequestCode will substitute
// the pinned gid so the HTTP fetch targets the correct (old) version
// rather than whatever new gid Steam asked the content server for.
//
// ignore.txt is reloaded automatically by LuaFileWatcher whenever it
// changes on disk, exactly like .lua files are.

namespace IgnoreList {

    // Load (or reload) ignore.txt files from all currently configured
    // Lua directories. Called at startup by dllmain after ParseDirectory
    // and by LuaFileWatcher on any ignore.txt add/modify event.
    void Reload(const std::vector<std::string>& directories);

    // Called by LuaFileWatcher when a single ignore.txt is changed.
    // `filePath` is the absolute path to the ignore.txt that changed.
    void ReloadFile(const std::string& filePath);

    // Called by LuaFileWatcher when an ignore.txt is deleted.
    void UnloadFile(const std::string& filePath);

    // Returns true when `appId` is listed in any loaded ignore.txt.
    [[nodiscard]] bool IsIgnored(AppId_t appId);

} // namespace IgnoreList