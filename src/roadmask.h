#pragma once

struct RwRaster;
struct IDirect3DTexture9;

bool InitRoadMask(void);
void RenderRoadMask(void);
RwRaster *GetRoadMaskRaster(void);
IDirect3DTexture9 *GetRoadMaskTexture(void);
void ShutdownRoadMask(void);
