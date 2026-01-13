# Interactive Test Runner - Changelog

## Version 2.0 - Terminal Width Fixes (2026-01-13)

### Fixed
- **Line wrapping issue**: All text now properly truncates to fit terminal width
- **Dynamic terminal sizing**: Interface adapts to any terminal size when resized
- **No more wrapped lines**: Long test names, descriptions, and output lines are truncated with "..." when necessary

### Technical Changes
- Added `truncate_str()` helper function to truncate strings to terminal width
- Updated all drawing functions to respect terminal width:
  - `draw_category_menu()`: Dynamic title width, truncated category names and descriptions
  - `draw_test_menu()`: Dynamic borders, truncated test names and descriptions
  - `draw_running()`: Truncated titles, info lines, and output lines
  - `draw_output_view()`: Truncated output lines and footer
- Title borders now adapt to terminal width (minimum 78 characters, max = terminal width)
- All UI elements respect `width - 1` to prevent wrapping at terminal edge

### Benefits
✅ Works on any terminal size (tested from 80x24 to larger)
✅ No line wrapping when terminal is resized
✅ Clean, professional appearance regardless of terminal dimensions
✅ Long test names/descriptions gracefully truncated with ellipsis
✅ Output lines properly fit terminal width

## Version 1.0 - Category Organization (2026-01-13)

### Added
- **Two-level menu system**: Category menu → Test menu
- **5 test categories**: Integration, Lobby & Session, Demo Lap, TLS & Security, Transport & Performance
- **29 total tests** organized by topic
- **Interactive navigation**: Backspace to go back between levels
- **Real-time output streaming** during test execution
- **Scrollable results view** with PageUp/PageDown support
- **Cancel capability**: Press 'C' to cancel running tests
- **Color-coded UI**: Clean visual feedback with cyan/blue/yellow/green colors

### Features
- Ncurses-like terminal interface
- Server requirement indicators `[S]` for tests needing running server
- Test descriptions shown when selected
- Dynamic test discovery from multiple test files
- Support for ignored tests with `--ignored` flag
- Keyboard-driven navigation (no mouse needed)
