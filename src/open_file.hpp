#pragma once

// Opening a finished download from the window (DOWNLOADS-UI.md, chunk H): a double-click, Open in
// its menu or Ctrl+Enter opens the file in its default app. A file that Windows would run as a
// program (an .exe, a script, an installer, a shortcut) is asked about first, since grab fetched
// it from a server and Windows itself won't warn: grab sets no mark of the web on its files. And
// only the file grab wrote opens: one moved, deleted or changed since says so instead.
// ShellExecuteEx itself is the window's (app.cpp).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace grab::open_file {

// A finished file to open: where it is and its size as grab wrote it (or found it there already,
// the server's copy).
struct Finished {
    std::filesystem::path path;
    std::uint64_t size = 0;
    bool operator==(const Finished&) const = default;
};

// What Windows says of a file type (facts_of): its program extensions (PATHEXT), the command its
// "open" verb runs ("" when it has none) and whether it is on Windows' own list of unsafe types
// (AssocIsDangerous).
struct Facts {
    std::string pathext;
    std::string open_command;
    bool dangerous = false;
};

// A file name's extension as Windows takes it: from its last dot, the trailing dots and spaces
// Windows drops left out, lower case; ".bat" for "x.BAT", "run.bat. " and ".bat" alike (a name
// that starts with its only dot is all extension to Windows, unlike std::filesystem); "" without
// a dot.
[[nodiscard]] std::string extension_of(const std::filesystem::path& file);

// Whether opening a file of extension `ext` (as extension_of gives it) runs a program rather than
// showing a document in its app: an extension of PATHEXT or of grab's own list (programs,
// installers, scripts and shortcuts: .exe .msi .ps1 .vbs .jar .lnk .url ...), a type Windows
// calls unsafe, or one whose "open" command runs the file itself ("%1" %*) or hands it to a
// script host or an interpreter (WScript, mshta, PowerShell, cmd, regedit, msiexec, Java,
// Python...).
[[nodiscard]] bool runs_program(std::string_view ext, const Facts& facts);

// The facts of `ext` on this PC: PATHEXT (Windows' default when unset), the "open" command the
// registry gives it (the user's choice of app included), AssocIsDangerous. Elsewhere: PATHEXT only.
[[nodiscard]] Facts facts_of(std::string_view ext);

// Whether opening `file` runs a program (runs_program of its extension with this PC's facts).
[[nodiscard]] bool runs_program(const std::filesystem::path& file);

// A finished file as it is on disk now.
enum class Found {
    ok,      // there, as grab wrote it
    gone,    // nothing there (moved or deleted), or it can't be looked at
    changed, // something else: another size, a folder or a link in its place
};
// Whether `f` is still what grab wrote: a regular file (not a folder or a link) of its size.
[[nodiscard]] Found check(const Finished& f);

} // namespace grab::open_file
