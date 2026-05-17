local GS build:
- local Linux `gs` build is normally run from this directory with `make -j4`
- when building Linux `gs` from Windows/WSL, use the `build-wsl` skill
- when building Android GS from Windows, use the `build-android` skill

specialized GS workflows:
- for WSL Linux `gs` launch, build, and MCP access from Windows, use the `gs-wsl-mcp-debug` skill
- for Radxa sync/build/launch, APFPV search/connect checks, and MCP menu automation, use the `gs-mcp-menu-debug` skill
