#pragma once

#include "imgui.h"

#if defined(_WIN32) || defined(_WIN64)
#include <d3d11.h>
#endif

namespace Preview {

bool Init(ID3D11Device* dev);
void Shutdown();
bool Ready();

void DrawPanel();

}
