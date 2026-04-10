file reference formatting:
- always use markdown links for file references
- always use absolute filesystem paths
- always start the path with `/`
- valid example: `[name](/d:/path/to/file.ts)`
- valid line example: `[name](/d:/path/to/file.ts#L123)`
- do not use `file://`
- do not use plain inline code for clickable file references

code style and comments:
- brackets `{}` are always placed on the next line
- every out-of-class function or method definition, and every class or struct definition, should start with:
  `//===================================================================================`
  `//===================================================================================`
- after these two lines, add a short English description comment explaining what it does
- do not add these separator lines or description comments to method declarations inside a class or struct body
- if you change an implementation, update any stale comments that describe that implementation, especially the leading comment above the definition and the leading comment above the class or struct declaration
- if testing or debugging reveals a non-obvious behavior, constraint, race, ordering requirement, or hardware/platform quirk, add a short English comment near the relevant implementation that explains the behavior and why the code is written that way
- do not leave important debug findings only in chat, commit messages, or temporary notes; preserve them in the code where a future engineer would otherwise repeat the same mistake

components_gs logging rule:
- in first-party `components_gs` code, always use shared `LOGD` / `LOGI` / `LOGW` / `LOGE` macros for logging
- do not use `__android_log_print`, `printf`, `fprintf`, or ad-hoc logging macros in first-party `components_gs` code
- if platform-specific logging behavior is needed, implement it inside `components_gs/shared/Log.h`, not at call sites
- treat vendored third-party code under `components_gs/imgui`, `components_gs/fmt`, and similar imported libraries as exceptions unless explicitly asked to modify them

