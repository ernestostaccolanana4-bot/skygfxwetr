#include "skygfx.h"
#include "wetRoads_skygfx.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool g_enabled = false;
    bool g_debugForceRain = false;
    bool g_maskDebug = false;
    float g_maskResolutionScale = 0.5f;
    float g_wetness = 0.0f;

    RwRaster* g_maskRaster = nullptr;
    RwRaster* g_maskZRaster = nullptr;
    int g_maskWidth = 0;
    int g_maskHeight = 0;

    float Clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    void UpdateWetness()
    {
        const float rain = g_debugForceRain ? 1.0f : Clamp01(CWeather__Rain);
        const float target = rain > 0.02f ? rain : 0.0f;
        const float step = std::max(CTimer__ms_fTimeStep, 0.0f) * 0.0025f;
        g_wetness += (target - g_wetness) * Clamp01(step);
        g_wetness = Clamp01(g_wetness);
    }

    void DestroyMaskTargets()
    {
        if (g_maskRaster)
            RwRasterDestroy(g_maskRaster);
        if (g_maskZRaster)
            RwRasterDestroy(g_maskZRaster);

        g_maskRaster = nullptr;
        g_maskZRaster = nullptr;
        g_maskWidth = 0;
        g_maskHeight = 0;
    }

    bool EnsureMaskTargets()
    {
        if (!Scene.camera)
            return false;

        RwRaster* sceneRaster = RwCameraGetRaster(Scene.camera);
        if (!sceneRaster)
            return false;

        const int width = std::max(1, static_cast<int>(sceneRaster->width * g_maskResolutionScale));
        const int height = std::max(1, static_cast<int>(sceneRaster->height * g_maskResolutionScale));

        if (g_maskRaster && g_maskWidth == width && g_maskHeight == height)
            return true;

        DestroyMaskTargets();

        g_maskRaster = RwRasterCreate(width, height, sceneRaster->depth, rwRASTERTYPECAMERATEXTURE);
        g_maskZRaster = RwRasterCreate(width, height, sceneRaster->depth, rwRASTERTYPEZBUFFER);
        if (!g_maskRaster || !g_maskZRaster)
        {
            DestroyMaskTargets();
            return false;
        }

        g_maskWidth = width;
        g_maskHeight = height;
        return true;
    }

    void RenderMaskDebugQuad()
    {
        if (!g_maskRaster || !Scene.camera)
            return;

        const float width = static_cast<float>(RwCameraGetRaster(Scene.camera)->width);
        const float height = static_cast<float>(RwCameraGetRaster(Scene.camera)->height);
        const float nearZ = RwIm2DGetNearScreenZ();

        RwIm2DVertex vertices[4] = {};
        vertices[0].x = 0.0f; vertices[0].y = 0.0f;
        vertices[1].x = 0.0f; vertices[1].y = height;
        vertices[2].x = width; vertices[2].y = height;
        vertices[3].x = width; vertices[3].y = 0.0f;

        for (int i = 0; i < 4; ++i)
        {
            vertices[i].z = nearZ;
            vertices[i].rhw = 1.0f;
            vertices[i].emissiveColor = 0xFFFFFFFF;
        }

        vertices[0].u = 0.0f; vertices[0].v = 0.0f;
        vertices[1].u = 0.0f; vertices[1].v = 1.0f;
        vertices[2].u = 1.0f; vertices[2].v = 1.0f;
        vertices[3].u = 1.0f; vertices[3].v = 0.0f;

        static const RwImVertexIndex indices[6] = { 0, 1, 2, 0, 2, 3 };

        CPostEffects::ImmediateModeRenderStatesStore();
        CPostEffects::ImmediateModeRenderStatesSet();
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, g_maskRaster);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERPOINT);
        RwD3D9SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        RwD3D9SetRenderState(D3DRS_ZENABLE, FALSE);
        RwD3D9SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        RwD3D9SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST, vertices, 4, indices, 6);
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        CPostEffects::ImmediateModeRenderStatesReStore();
    }
}

void WetRoadsSkyGfx_SetEnabled(bool enabled)
{
    g_enabled = enabled;
}

void WetRoadsSkyGfx_SetDebugForceRain(bool enabled)
{
    g_debugForceRain = enabled;
}

void WetRoadsSkyGfx_SetMaskDebug(bool enabled)
{
    g_maskDebug = enabled;
}

void WetRoadsSkyGfx_SetMaskResolutionScale(float scale)
{
    g_maskResolutionScale = std::clamp(scale, 0.25f, 1.0f);
    DestroyMaskTargets();
}

float WetRoadsSkyGfx_GetWetness()
{
    return g_wetness;
}

void WetRoadsSkyGfx_RenderRoadMask()
{
    if (!g_enabled || !Scene.camera || !EnsureMaskTargets())
        return;

    RwRaster* sceneRaster = RwCameraGetRaster(Scene.camera);
    RwRaster* sceneZRaster = RwCameraGetZRaster(Scene.camera);
    if (!sceneRaster)
        return;

    // Render roads into a separate camera raster. The original scene target and
    // depth raster are restored before returning to SkyGFX.
    RwCameraEndUpdate(Scene.camera);
    RwCameraSetRaster(Scene.camera, g_maskRaster);
    RwCameraSetZRaster(Scene.camera, g_maskZRaster);

    RwRGBA black = { 0, 0, 0, 255 };
    RwCameraClear(Scene.camera, &black, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
    RwCameraBeginUpdate(Scene.camera);

    CRenderer__RenderRoads();

    RwCameraEndUpdate(Scene.camera);
    RwCameraSetRaster(Scene.camera, sceneRaster);
    RwCameraSetZRaster(Scene.camera, sceneZRaster);
    RwCameraBeginUpdate(Scene.camera);

    if (g_maskDebug)
        RenderMaskDebugQuad();
}

void WetRoadsSkyGfx_RenderAfterScene()
{
    if (!g_enabled)
        return;

    UpdateWetness();
    WetRoadsSkyGfx_RenderRoadMask();

    if (g_wetness <= 0.001f)
        return;

    // The mask is now available for the future selective composite. The first
    // implementation intentionally stops here unless mask-debug is enabled.
    // Next stage: copy the scene raster and composite using a ps_2_a shader.
}
