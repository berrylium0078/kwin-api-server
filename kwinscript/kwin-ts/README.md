# kwin-ts

TypeScript definitions converted from KWin C++ Doxygen documentation.

Generated on: 2026-01-20T11:45:51.204Z

[Converter repo](https://github.com/drendog/kwin-types-parser-ts)

## Manual Patches

- Fixed typo in `tsconfig.json`: "declarationOnly" -> "emitDeclarationOnly"
- `moduleResolution` set to bundler, since KWin scripts are single JS files
- Unescaped the characters in `index.d.ts`
- Added QTimer class definition
- Added `KWin.WorkspaceWrapper.windowList()` and `KWin.WorkspaceWrapper.slotToggleMaximize()`
  — scripting-injected members missing from the generated output