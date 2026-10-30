// SF-IDE bridge. When clil runs inside the SF-IDE editor, SF-IDE sets
// SFIDE_SOCKET to a private Unix socket. File reads and edits then go
// through the editor: reads see unsaved changes, and edits appear live in
// the open tab (undoable there) before the editor saves them.
#pragma once
#include <string>

bool ide_attached();
// Contents of `path` as open in the editor, including unsaved edits.
// False when there is no editor or the file is not open there.
bool ide_read(const std::string& path, std::string& data);
// Gives the new contents of `path` to the editor, which applies and saves
// them. False when not handled (no editor): write the file yourself; `err`
// is set when the editor tried and failed.
bool ide_write(const std::string& path, const std::string& data, std::string& err);
// What the user has open in the editor, appended to their message
// ("\n\n[SF-IDE: ...]"); empty without an editor.
std::string ide_context();
