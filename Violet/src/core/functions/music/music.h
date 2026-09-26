#pragma once
#include <cstdint>
#if defined(_WIN32) || defined(_WIN64)
#include <d3d11.h>
#endif

namespace Music {
// Violet music deck: system media (Spotify first) + LRCLIB synced lyrics,
// painted in the menu theme. Worker thread polls, panel thread only reads.
void Loop();   // spawn in App::Run like the other loops
void Stop();   // join
void DrawPanel(ID3D11Device* dev); // each frame from the overlay loop
} // namespace Music
