#pragma once

void WetRoadsSkyGfx_RenderAfterScene();
void WetRoadsSkyGfx_RenderRoadMask();

void WetRoadsSkyGfx_SetEnabled(bool enabled);
void WetRoadsSkyGfx_SetDebugForceRain(bool enabled);
void WetRoadsSkyGfx_SetMaskDebug(bool enabled);
void WetRoadsSkyGfx_SetMaskResolutionScale(float scale);
float WetRoadsSkyGfx_GetWetness();
