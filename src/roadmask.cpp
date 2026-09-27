#include "skygfx.h"
#include "roadmask.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
	constexpr int ROADMASK_NUM_AREAS = 64 + 8;
	constexpr float ROADMASK_NODE_COORD_SCALE = 1.0f / 8.0f;
	constexpr float ROADMASK_QUERY_RADIUS = 100.0f;
	constexpr float ROADMASK_EXTRA_WIDTH = 2.5f;
	constexpr float ROADMASK_Z_BIAS = 0.05f;
	constexpr int ROADMASK_MAX_QUADS = 4096;

	struct CNodeAddressSA
	{
		uint16 region;
		uint16 index;
	};
	static_assert(sizeof(CNodeAddressSA) == 0x4, "Wrong size: CNodeAddressSA");

	struct CPathNodeSA
	{
		CPathNodeSA *pNext;
		CPathNodeSA *pPrevious;
		int16 coorsX;
		int16 coorsY;
		int16 coorsZ;
		int16 distanceToTarget;
		int16 indexAdjacentNodes;
		CNodeAddressSA address;
		uint8 width;
		uint8 group;
		uint8 numberAdjNodes : 4;
		uint8 onDeadEnd : 1;
		uint8 switchedOff : 1;
		uint8 roadBlock : 1;
		uint8 waterNode : 1;
		uint8 switchedOffOriginal : 1;
		uint8 alreadyFound : 1;
		uint8 dontWanderHere : 1;
		uint8 interiorNode : 1;
		uint8 speed : 2;
		uint8 dummy : 2;
		uint8 density : 4;
		uint8 specialFunction : 4;
	};
	static_assert(sizeof(CPathNodeSA) == 0x1C, "Wrong size: CPathNodeSA");

	struct CPathFindSA
	{
		CNodeAddressSA info;
		CPathNodeSA *m_apNodesSearchLists[512];
		CPathNodeSA *m_pPathNodes[ROADMASK_NUM_AREAS];
		void *m_pNaviNodes[ROADMASK_NUM_AREAS];
		CNodeAddressSA *m_pNodeLinks[ROADMASK_NUM_AREAS];
		uint8 *m_pLinkLengths[ROADMASK_NUM_AREAS];
		void *m_pPathIntersections[ROADMASK_NUM_AREAS];
		void *m_pNaviLinks[64];
		void *field_EA4[64];
		uint32 m_dwNumNodes[ROADMASK_NUM_AREAS];
		uint32 m_dwNumVehicleNodes[ROADMASK_NUM_AREAS];
	};
	static_assert(offsetof(CPathFindSA, m_pPathNodes) == 0x804, "Wrong offset: m_pPathNodes");
	static_assert(offsetof(CPathFindSA, m_pNodeLinks) == 0xA44, "Wrong offset: m_pNodeLinks");
	static_assert(offsetof(CPathFindSA, m_dwNumVehicleNodes) == 0x10C4, "Wrong offset: m_dwNumVehicleNodes");

	struct Vec4
	{
		float x, y, z, w;
	};

	RwRaster *roadMaskRaster;
	RwRaster *roadMaskZRaster;
	IDirect3DTexture9 *roadMaskTexture;
	bool roadMaskInitLogged;
	bool roadMaskPathWarningLogged;

	std::vector<RwIm2DVertex> roadMaskVertices;
	std::vector<RwImVertexIndex> roadMaskIndices;

	inline CPathFindSA *GetThePaths(void)
	{
		return *(CPathFindSA**)0x40CA27;
	}

	inline CVector NodeToWorld(const CPathNodeSA &node)
	{
		CVector out;
		out.x = node.coorsX * ROADMASK_NODE_COORD_SCALE;
		out.y = node.coorsY * ROADMASK_NODE_COORD_SCALE;
		out.z = node.coorsZ * ROADMASK_NODE_COORD_SCALE;
		return out;
	}

	inline float Dot2D(float ax, float ay, float bx, float by)
	{
		return ax * bx + ay * by;
	}

	inline bool IsValidAddress(const CNodeAddressSA &address)
	{
		return address.region < ROADMASK_NUM_AREAS && address.index != 0xFFFF;
	}

	Vec4 MulPoint(const D3DMATRIX &m, const Vec4 &v)
	{
		Vec4 out;
		out.x = v.x * m._11 + v.y * m._21 + v.z * m._31 + v.w * m._41;
		out.y = v.x * m._12 + v.y * m._22 + v.z * m._32 + v.w * m._42;
		out.z = v.x * m._13 + v.y * m._23 + v.z * m._33 + v.w * m._43;
		out.w = v.x * m._14 + v.y * m._24 + v.z * m._34 + v.w * m._44;
		return out;
	}

	bool WorldToScreen(const CVector &world, int width, int height, const D3DMATRIX &view, const D3DMATRIX &proj,
		float &sx, float &sy, float &sz, float &rhw)
	{
		const Vec4 in = { world.x, world.y, world.z, 1.0f };
		const Vec4 viewPos = MulPoint(view, in);
		const Vec4 clipPos = MulPoint(proj, viewPos);

		if(clipPos.w <= 0.0001f)
			return false;

		const float invW = 1.0f / clipPos.w;
		const float ndcX = clipPos.x * invW;
		const float ndcY = clipPos.y * invW;
		const float ndcZ = clipPos.z * invW;

		sx = (ndcX * 0.5f + 0.5f) * width;
		sy = (-ndcY * 0.5f + 0.5f) * height;
		sz = ndcZ;
		rhw = invW;
		return true;
	}

	void ReleaseRoadMaskTexture(void)
	{
		if(roadMaskTexture){
			roadMaskTexture->Release();
			roadMaskTexture = nil;
		}
	}

	void AcquireRoadMaskTextureFromCurrentTarget(void)
	{
		ReleaseRoadMaskTexture();
		if(d3d9device == nil)
			return;

		IDirect3DSurface9 *surface = nil;
		if(FAILED(d3d9device->GetRenderTarget(0, &surface)) || surface == nil)
			return;

		IDirect3DTexture9 *texture = nil;
		surface->GetContainer(IID_IDirect3DTexture9, (void**)&texture);
		surface->Release();

		roadMaskTexture = texture;
	}

	void LogRoadMaskMessage(const char *msg)
	{
		OutputDebugStringA(msg);
		OutputDebugStringA("\n");
	}

	bool EnsureRoadMaskRaster(void)
	{
		RwCamera *camera = Scene.camera;
		if(camera == nil)
			return false;

		RwRaster *cameraRaster = RwCameraGetRaster(camera);
		if(cameraRaster == nil)
			return false;

		if(roadMaskRaster != nil &&
		   roadMaskRaster->width == cameraRaster->width &&
		   roadMaskRaster->height == cameraRaster->height)
			return true;

		if(roadMaskRaster)
			RwRasterDestroy(roadMaskRaster);
		if(roadMaskZRaster)
			RwRasterDestroy(roadMaskZRaster);
		roadMaskRaster = RwRasterCreate(cameraRaster->width, cameraRaster->height, cameraRaster->depth, rwRASTERTYPECAMERATEXTURE);
		roadMaskZRaster = RwRasterCreate(cameraRaster->width, cameraRaster->height, 0, rwRASTERTYPEZBUFFER);
		ReleaseRoadMaskTexture();

		if(roadMaskRaster == nil || roadMaskZRaster == nil){
			if(!roadMaskInitLogged){
				LogRoadMaskMessage("WetRoads: Failed to create road mask raster");
				roadMaskInitLogged = true;
			}
			return false;
		}

		roadMaskInitLogged = true;
		LogRoadMaskMessage("WetRoads: Road mask raster initialized");
		return true;
	}

	void PushQuad(const CVector &a, const CVector &b, float halfWidth,
		const D3DMATRIX &view, const D3DMATRIX &proj, int width, int height)
	{
		const float dx = b.x - a.x;
		const float dy = b.y - a.y;
		const float lenSq = Dot2D(dx, dy, dx, dy);
		if(lenSq < 0.0001f)
			return;

		const float invLen = 1.0f / sqrtf(lenSq);
		const float px = -dy * invLen;
		const float py = dx * invLen;

		CVector world[4] = {
			{ a.x + px * halfWidth, a.y + py * halfWidth, a.z + ROADMASK_Z_BIAS },
			{ a.x - px * halfWidth, a.y - py * halfWidth, a.z + ROADMASK_Z_BIAS },
			{ b.x - px * halfWidth, b.y - py * halfWidth, b.z + ROADMASK_Z_BIAS },
			{ b.x + px * halfWidth, b.y + py * halfWidth, b.z + ROADMASK_Z_BIAS },
		};

		RwIm2DVertex v[4];
		for(int i = 0; i < 4; i++){
			float sx, sy, sz, rhw;
			if(!WorldToScreen(world[i], width, height, view, proj, sx, sy, sz, rhw))
				return;
			v[i].x = sx;
			v[i].y = sy;
			v[i].z = sz;
			v[i].rhw = rhw;
			v[i].u = 0.0f;
			v[i].v = 0.0f;
			v[i].emissiveColor = 0xFFFFFFFF;
		}

		if(roadMaskVertices.size() > 65530)
			return;

		const RwImVertexIndex base = (RwImVertexIndex)roadMaskVertices.size();
		roadMaskVertices.insert(roadMaskVertices.end(), &v[0], &v[4]);
		roadMaskIndices.push_back(base + 0);
		roadMaskIndices.push_back(base + 1);
		roadMaskIndices.push_back(base + 2);
		roadMaskIndices.push_back(base + 0);
		roadMaskIndices.push_back(base + 2);
		roadMaskIndices.push_back(base + 3);
	}

	void BuildRoadMaskGeometry(const CPathFindSA &paths, const CVector &center, float radius,
		const D3DMATRIX &view, const D3DMATRIX &proj, int width, int height)
	{
		roadMaskVertices.clear();
		roadMaskIndices.clear();
		roadMaskVertices.reserve(ROADMASK_MAX_QUADS * 4);
		roadMaskIndices.reserve(ROADMASK_MAX_QUADS * 6);

		const float radiusSq = radius * radius;

		for(uint16 area = 0; area < ROADMASK_NUM_AREAS; area++){
			CPathNodeSA *areaNodes = paths.m_pPathNodes[area];
			CNodeAddressSA *areaLinks = paths.m_pNodeLinks[area];
			if(areaNodes == nil || areaLinks == nil)
				continue;

			const uint32 nodeCount = paths.m_dwNumVehicleNodes[area];
			for(uint32 nodeId = 0; nodeId < nodeCount; nodeId++){
				if((int)roadMaskIndices.size() >= ROADMASK_MAX_QUADS * 6)
					return;

				const CPathNodeSA &node = areaNodes[nodeId];
				if(node.waterNode || node.switchedOff)
					continue;

				const CVector nodePos = NodeToWorld(node);
				const float relx = nodePos.x - center.x;
				const float rely = nodePos.y - center.y;
				if(Dot2D(relx, rely, relx, rely) > radiusSq)
					continue;

				const uint32 thisKey = (uint32(area) << 16) | uint32(nodeId);
				if(node.indexAdjacentNodes < 0)
					continue;
				for(uint32 linkIndex = 0; linkIndex < node.numberAdjNodes; linkIndex++){
					const CNodeAddressSA &linkAddress = areaLinks[node.indexAdjacentNodes + linkIndex];
					if(!IsValidAddress(linkAddress))
						continue;
					if(paths.m_pPathNodes[linkAddress.region] == nil)
						continue;
					if(linkAddress.index >= paths.m_dwNumVehicleNodes[linkAddress.region])
						continue;

					const uint32 otherKey = (uint32(linkAddress.region) << 16) | uint32(linkAddress.index);
					if(otherKey <= thisKey)
						continue;

					const CPathNodeSA &other = paths.m_pPathNodes[linkAddress.region][linkAddress.index];
					if(other.waterNode || other.switchedOff)
						continue;

					const CVector otherPos = NodeToWorld(other);
					const float widthA = std::max(node.width * ROADMASK_NODE_COORD_SCALE, 1.5f);
					const float widthB = std::max(other.width * ROADMASK_NODE_COORD_SCALE, 1.5f);
					const float halfWidth = std::max((widthA + widthB) * 0.5f + ROADMASK_EXTRA_WIDTH, 2.0f);
					PushQuad(nodePos, otherPos, halfWidth, view, proj, width, height);
				}
			}
		}
	}

	void DrawRoadMaskGeometry(void)
	{
		if(roadMaskIndices.empty())
			return;

		void *prevTexture, *prevVertexAlpha, *prevZTest, *prevZWrite, *prevSrcBlend, *prevDstBlend, *prevFog;
		RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &prevTexture);
		RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &prevVertexAlpha);
		RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &prevZTest);
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &prevZWrite);
		RwRenderStateGet(rwRENDERSTATESRCBLEND, &prevSrcBlend);
		RwRenderStateGet(rwRENDERSTATEDESTBLEND, &prevDstBlend);
		RwRenderStateGet(rwRENDERSTATEFOGENABLE, &prevFog);

		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDONE);
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDZERO);
		RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);

		RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST,
			roadMaskVertices.data(), (int)roadMaskVertices.size(),
			roadMaskIndices.data(), (int)roadMaskIndices.size());

		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, prevTexture);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, prevVertexAlpha);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, prevZTest);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, prevZWrite);
		RwRenderStateSet(rwRENDERSTATESRCBLEND, prevSrcBlend);
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, prevDstBlend);
		RwRenderStateSet(rwRENDERSTATEFOGENABLE, prevFog);
	}

	void DrawRoadMaskDebugPreview(void)
	{
		if(roadMaskRaster == nil)
			return;
		if((GetAsyncKeyState(VK_F7) & 0x8000) == 0)
			return;

		const float scale = 0.3f;
		const float w = roadMaskRaster->width * scale;
		const float h = roadMaskRaster->height * scale;
		const float x0 = 24.0f;
		const float y0 = 24.0f;

		RwIm2DVertex v[4];
		v[0].x = x0;      v[0].y = y0;
		v[1].x = x0;      v[1].y = y0 + h;
		v[2].x = x0 + w;  v[2].y = y0 + h;
		v[3].x = x0 + w;  v[3].y = y0;
		for(int i = 0; i < 4; i++){
			v[i].z = RwIm2DGetNearScreenZ();
			v[i].rhw = 1.0f / RwCameraGetNearClipPlane(Scene.camera);
			v[i].emissiveColor = 0xFFFFFFFF;
		}
		v[0].u = 0.0f; v[0].v = 0.0f;
		v[1].u = 0.0f; v[1].v = 1.0f;
		v[2].u = 1.0f; v[2].v = 1.0f;
		v[3].u = 1.0f; v[3].v = 0.0f;

		RwImVertexIndex idx[6] = { 0, 1, 2, 0, 2, 3 };
		void *prevTexture, *prevFog, *prevZTest, *prevZWrite, *prevVertexAlpha;
		RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &prevTexture);
		RwRenderStateGet(rwRENDERSTATEFOGENABLE, &prevFog);
		RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &prevZTest);
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &prevZWrite);
		RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &prevVertexAlpha);

		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, roadMaskRaster);
		RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
		RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST, v, 4, idx, 6);

		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, prevTexture);
		RwRenderStateSet(rwRENDERSTATEFOGENABLE, prevFog);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, prevZTest);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, prevZWrite);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, prevVertexAlpha);
	}
}

bool
InitRoadMask(void)
{
	return EnsureRoadMaskRaster();
}

void
RenderRoadMask(void)
{
	if(!EnsureRoadMaskRaster())
		return;

	RwCamera *camera = Scene.camera;
	if(camera == nil)
		return;

	RwRaster *cameraRaster = RwCameraGetRaster(camera);
	RwRaster *sceneZRaster = RwCameraGetZRaster(camera);
	if(cameraRaster == nil)
		return;

	RwRaster *renderZRaster = roadMaskZRaster;
	bool useSceneDepth = false;
	if(sceneZRaster &&
	   sceneZRaster->width == roadMaskRaster->width &&
	   sceneZRaster->height == roadMaskRaster->height){
		renderZRaster = sceneZRaster;
		useSceneDepth = true;
	}

	RwCameraEndUpdate(camera);
	RwCameraSetRaster(camera, roadMaskRaster);
	RwCameraSetZRaster(camera, renderZRaster);

	RwRGBA clearColor = { 0, 0, 0, 255 };
	RwCameraClear(camera, &clearColor, useSceneDepth ? rwCAMERACLEARIMAGE : (rwCAMERACLEARIMAGE | rwCAMERACLEARZ));
	RwCameraBeginUpdate(camera);

	AcquireRoadMaskTextureFromCurrentTarget();

	CPathFindSA *paths = GetThePaths();
	if(paths && d3d9device){
		D3DMATRIX view, proj;
		d3d9device->GetTransform(D3DTS_VIEW, &view);
		d3d9device->GetTransform(D3DTS_PROJECTION, &proj);

		const CVector center = TheCamera.GetPosition();
		BuildRoadMaskGeometry(*paths, center, ROADMASK_QUERY_RADIUS, view, proj, roadMaskRaster->width, roadMaskRaster->height);
		DrawRoadMaskGeometry();
	}else if(!roadMaskPathWarningLogged){
		LogRoadMaskMessage("WetRoads: CPathFind unavailable, road mask will stay black");
		roadMaskPathWarningLogged = true;
	}

	RwCameraEndUpdate(camera);
	RwCameraSetRaster(camera, cameraRaster);
	RwCameraSetZRaster(camera, sceneZRaster);
	RwCameraBeginUpdate(camera);

	DrawRoadMaskDebugPreview();
}

RwRaster *
GetRoadMaskRaster(void)
{
	return roadMaskRaster;
}

IDirect3DTexture9 *
GetRoadMaskTexture(void)
{
	return roadMaskTexture;
}

void
ShutdownRoadMask(void)
{
	ReleaseRoadMaskTexture();
	if(roadMaskRaster){
		RwRasterDestroy(roadMaskRaster);
		roadMaskRaster = nil;
	}
	if(roadMaskZRaster){
		RwRasterDestroy(roadMaskZRaster);
		roadMaskZRaster = nil;
	}
}
