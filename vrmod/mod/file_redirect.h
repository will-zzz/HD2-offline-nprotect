#pragma once
// File redirect for the HD2 VR mod.
// The game runs from the pristine Steam install (C:). Reads of files under its data folder are
// redirected to the same relative path in the modified copy (D:\HD2-Offline\data) whenever that
// file exists there. The Steam install itself is never written to.
namespace hd2vr {
    // Hooks CreateFileW and NtCreateFile. Call once from the injected worker thread.
    bool InstallFileRedirect();
}
