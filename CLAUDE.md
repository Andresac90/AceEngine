# AceEngine

Read `docs/HANDOFF.md` before making changes: it has the owner's decisions,
the architecture, what is verified, the pitfalls, and the ordered roadmap.

- Build: `cmake -S . -B build -G Ninja && cmake --build build`
- Test: `build/bin/ace_tests`
- Verify rendering: run `ace_player`/`ace_editor` with `--screenshot <png>` on
  both `--backend metal` and `--backend vulkan`, and look at the image.
- Commit locally; never push (the owner pushes). Ask before installing tools.
- Commit messages must not contain a `Co-Authored-By` line (or any other AI
  attribution): the owner wants to be the only author shown.
- Follow the working rules in HANDOFF section 5 (minimal code, `shortcut:`
  comments for deliberate debt, one small test for branching logic).
