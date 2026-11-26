.pragma library

// Display glyphs for keyboard modifiers, per platform. Qt maps the Ctrl sequences to ⌘ on
// macOS and Ctrl on Windows, so the shortcuts already fire — only the labels differ.
function mod() { return Qt.platform.os === "osx" ? "⌘" : "Ctrl" }
function opt() { return Qt.platform.os === "osx" ? "⌥" : "Alt" }
