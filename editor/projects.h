// Finding, creating and remembering projects, for an editor started from Finder
// (no command line) as much as from a terminal.
#pragma once
#include "core/base.h"

struct SDL_Window;

namespace ace {

bool isProject(const std::string& dir);                      // has a project.json
bool createProject(const std::string& dir);                  // starter project; never overwrites one
std::string lastProject();                                   // "" if none remembered (or it is gone)
void rememberProject(const std::string& dir);                // stored in the user's settings folder
std::string editorSettingsDir();                             // per-user folder for editor files, ends with '/'
// Asks with the system folder picker until the user picks a project, or an
// empty folder (a new project is created there). "" if they cancel.
std::string chooseProject(SDL_Window* parent);

} // namespace ace
