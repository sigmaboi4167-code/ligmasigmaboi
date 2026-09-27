#pragma once
// Violet launcher — our own UI instead of the cmd prompt.
// Win32 + DirectX11 + Dear ImGui: the exact same stack the overlay already
// uses (Violet/ext/imgui), so zero new dependencies.
// Shows version check, changelog, download progress and a Launch button,
// then hands off to App::Run.
namespace Launcher {
// Returns true to continue into App::Run, false to exit (updated/quit).
bool Run();
} // namespace Launcher
