#version 430 core

#define RKO_OneWay   0
#define RKO_ThreeWay 1

#define WARP_NONE 0
#define WARP_POS  1
#define WARP_UV   2
#define WARP_BOTH 3

#define LIGHTK_Direction 0
#define LIGHTK_Position  1
#define LIGHTK_Frustrum  2
#define LIGHTK_Spot      3

#define MAX_LIGHTS 256
#define MAX_SHADOWS 255
#define MAX_OBJECT_SHADOWS 12

#define TRLK_Relight 0
#define TRLK_Baked   1
#define TRLK_Dynamic 2
#define TRLK_Quick   3

#define FOG_NONE 0
#define FOG_PS2  1
#define FOG_PS3  2


#ifdef CANE_DIAG_SHADER
uniform int caneVert;   // debug: 1 = no vertex shadow loop, 2 = no lighting, 4 = no skinning, 8 = no static relight, 16 = no dynamic lights
#define CANE_VERT_OFF(bit) ((caneVert & (bit)) != 0)
bool caneRelightMismatch = false;
bool caneBakeWasZero = false;
#else
#define CANE_VERT_OFF(bit) false
#endif

layout (location = 0) in vec3  vertex;
layout (location = 1) in vec3  normal;
layout (location = 2) in vec4  color;
layout (location = 3) in vec2  uv;
layout (location = 4) in uvec4 boneIndices;
layout (location = 5) in vec4  boneWeights;
#ifdef CANE_VERTEX_UBO_FALLBACK
// Static lighting baked once per relight by a transform-feedback pass (see DrawGlob); read back here
// instead of recomputing the light loops for every vertex of every draw. 36 bytes per vertex.
layout (location = 6) in float bakeAmbient;
layout (location = 7) in vec4  bakeMidtone;
layout (location = 8) in vec4  bakeLight;
uniform int caneBake;   // 1 while the bake pass is running: static lighting only, no dynamic lights
#define CANE_BAKING (caneBake != 0)
#else
#define CANE_BAKING false
#endif

struct SWP // Scene world properties
{
    float uShadow;
    float uMidtone;
    int   fogType;
    float fogNear;
    float fogFar;
    float fogMax;
    vec4  fogColor;
}; uniform SWP swp;

struct LIGHT
{
    int   lightk;
    int   fExcludeDynamicObjects;
    int   fActive;
    int   fDynamic;
    vec4  pos;
    vec4  dir;
    vec4  color;
    float constant;
    float invDst;
    float pad4;
    float dst;
    vec4  ru;
    vec4  du;
    mat4  matFrustrum;
    vec4  falloffScale;
    vec4  falloffBias;
};

struct SHADOW
{
    mat4  matWorldToUv;
    mat4  matClipToUv;
    vec4  rgba;
    float wMin;
    float wMax;
    float gReserved;
    float wFadeMin;
    int   textureSlot;
    int   clampS;
    int   clampT;
    int   padTexture2;
    vec4  posEffect;
    float sRadiusEffect;
    int   fDynamic;
    int   shdk;
    int   _pad1;
    vec4  normalCast;
};

struct MATERIAL
{
    float ambient;
    vec4  midtone;
    vec4  light;
};

#ifdef CANE_VERTEX_UBO_FALLBACK
// Fallback: the camera travels as plain uniforms. Their values are captured with each draw, so rewriting
// them for the next pass can never change a draw that is already queued (Adreno showed one-frame
// glitches with the shared camera uniform block).
uniform mat4 caneCmWorldToClip;
uniform vec4 caneCmCameraPos;
struct CMGL_T
{
    mat4 matWorldToClip;
    vec4 cameraPos;
};
#define cm CMGL_T(caneCmWorldToClip, caneCmCameraPos)
#else
layout(std140, binding = 0) uniform CMGL
{
    mat4 matWorldToClip;
    vec4 cameraPos;
} cm;
#endif

layout(std140, binding = 1) uniform RO
{
    mat4  model; 
    float uAlpha;
    float uFog;
    float darken;
    int   grfglob;
    int   blotTvLight;
    int   warpType;
    int   warpCmat;
    int   warpCvtx;
    mat4  amatDpos[4];
    mat4  amatDuv[4];
    int   fDynamic;
    float sRadius;
    int   fDynamicLight;
    int   trlk;
    vec4  posCenter;
    // Fallback per-draw light lists built on the CPU (all zero = not built, scan everything):
    //   x = static list length + 1, y = dynamic list length + 1, z = static list kind
    //   (0 = relight list over the whole level table, 1 = camera-zone active static lights).
    ivec4 relightHdr;
    ivec4 relightIdx[7];  // [0..15] static list, [16..27] dynamic list
} op;

#ifdef CANE_VERTEX_UBO_FALLBACK
// §8.1 vertex-SSBO fallback: the light/shadow tables become std140 uniform
// blocks (the CPU layouts are already std140-compatible; sized arrays keep
// .length() working) and the per-mesh arrays become data textures sampled
// with texelFetch (sampler units fixed in gl.cpp). Index-list access goes
// through the macros below, which collapse to the original expressions on
// the desktop path.
// The light table is a data texture here, not a uniform block: with a 256-entry LIGHT array in a
// std140 block the Adreno driver copies the whole block (~50 KB) on every draw (it was >50% of the
// game thread's CPU time). Each light is 13 RGBA32I texels (raw LIGHTBLK bits), one light per row.
layout(std140, binding = 2) uniform LIGHTBLK
{
    int numLights;
};
uniform isampler2D lightTex;   // unit 10

LIGHT FetchLight(int i)
{
    LIGHT L;
    ivec4 t0 = texelFetch(lightTex, ivec2(0, i), 0);
    L.lightk = t0.x;
    L.fExcludeDynamicObjects = t0.y;
    L.fActive = t0.z;
    L.fDynamic = t0.w;
    L.pos = intBitsToFloat(texelFetch(lightTex, ivec2(1, i), 0));
    L.dir = intBitsToFloat(texelFetch(lightTex, ivec2(2, i), 0));
    L.color = intBitsToFloat(texelFetch(lightTex, ivec2(3, i), 0));
    vec4 t4 = intBitsToFloat(texelFetch(lightTex, ivec2(4, i), 0));
    L.constant = t4.x;
    L.invDst = t4.y;
    L.pad4 = t4.z;
    L.dst = t4.w;
    L.ru = intBitsToFloat(texelFetch(lightTex, ivec2(5, i), 0));
    L.du = intBitsToFloat(texelFetch(lightTex, ivec2(6, i), 0));
    L.matFrustrum = mat4(
        intBitsToFloat(texelFetch(lightTex, ivec2(7, i), 0)),
        intBitsToFloat(texelFetch(lightTex, ivec2(8, i), 0)),
        intBitsToFloat(texelFetch(lightTex, ivec2(9, i), 0)),
        intBitsToFloat(texelFetch(lightTex, ivec2(10, i), 0)));
    L.falloffScale = intBitsToFloat(texelFetch(lightTex, ivec2(11, i), 0));
    L.falloffBias = intBitsToFloat(texelFetch(lightTex, ivec2(12, i), 0));
    return L;
}
#define LIGHTAT(i) FetchLight(i)

layout(std140, binding = 3) uniform ACTIVELIGHTS
{
    int numStaticLights;
    ivec4 staticLightIdx4[MAX_LIGHTS / 4];
    int numDynamicLights;
    ivec4 dynamicLightIdx4[MAX_LIGHTS / 4];
};

layout(std140, binding = 4) uniform SHADOWBLK
{
    int numLevelShadows;
    SHADOW shadows[MAX_SHADOWS];
};

layout(std140, binding = 5) uniform ACTIVESHADOWS
{
    int numActiveShadows;
    ivec4 shadowIdx4[(MAX_SHADOWS + 3) / 4];
};

layout(std140, binding = 13) uniform TVLIGHTBLK
{
    LIGHT tvLights[2];
};

uniform sampler2D warpStateTex;
uniform sampler2D bonesTex;
uniform sampler2D poseDposTex;
uniform sampler2D poseDnormalTex;

#define STATIC_LIGHT_IDX(i) (staticLightIdx4[(i) >> 2][(i) & 3])
#define DYN_LIGHT_IDX(i)    (dynamicLightIdx4[(i) >> 2][(i) & 3])
#define SHADOW_IDX(i)       (shadowIdx4[(i) >> 2][(i) & 3])
#define WARPSTATE_AT(i)     texelFetch(warpStateTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define POSEPOS_AT(i)       texelFetch(poseDposTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define POSENORMAL_AT(i)    texelFetch(poseDnormalTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define BONE_AT(i)          mat4(texelFetch(bonesTex, ivec2(0, int(i)), 0), \
                                 texelFetch(bonesTex, ivec2(1, int(i)), 0), \
                                 texelFetch(bonesTex, ivec2(2, int(i)), 0), \
                                 texelFetch(bonesTex, ivec2(3, int(i)), 0))
#define WARPSTATE_LEN       (textureSize(warpStateTex, 0).x * textureSize(warpStateTex, 0).y)
#define POSEPOS_LEN         (textureSize(poseDposTex, 0).x * textureSize(poseDposTex, 0).y)
#define MAX_POSES           64
uniform float poseWeights[MAX_POSES];
#define POSEWEIGHTS_LEN     MAX_POSES
#define POSENORMAL_LEN      (textureSize(poseDnormalTex, 0).x * textureSize(poseDnormalTex, 0).y)
#define BONE_LEN            (textureSize(bonesTex, 0).y)
#else

layout(std430, binding = 2) readonly buffer LIGHTBLK
{   
    int numLights;
    int pad[3];
    LIGHT lights[];
};
#define LIGHTAT(i) lights[i]

layout(std430, binding = 3) readonly buffer ACTIVELIGHTS
{
    int numStaticLights;
    int staticLightIndices[MAX_LIGHTS];
    int numDynamicLights;
    int dynamicLightIndices[MAX_LIGHTS];
};

layout(std430, binding = 4) readonly buffer SHADOWBLK
{
    int numLevelShadows;
    int padShadowBlk[3];
    SHADOW shadows[];
};

layout(std430, binding = 5) readonly buffer ACTIVESHADOWS
{
    int numActiveShadows;
    int shadowIndices[MAX_SHADOWS];
};

layout(std430, binding = 6) readonly buffer WARPSTATE
{
    vec4 warpState[];
};

layout(std430, binding = 9) readonly buffer BONEBLK
{
    mat4 boneMatrices[];
};

layout(std430, binding = 10) readonly buffer POSEPOSBLK
{
    vec4 poseDpos[];
};

layout(std430, binding = 11) readonly buffer POSENORMALBLK
{
    vec4 poseDnormal[];
};

layout(std430, binding = 14) readonly buffer POSEWEIGHTBLK
{
    float poseWeights[];
};

layout(std430, binding = 13) readonly buffer TVLIGHTBLK
{
    LIGHT tvLights[];
};

#define STATIC_LIGHT_IDX(i) (staticLightIndices[i])
#define DYN_LIGHT_IDX(i)    (dynamicLightIndices[i])
#define SHADOW_IDX(i)       (shadowIndices[i])
#define WARPSTATE_AT(i)     (warpState[(i)])
#define POSEPOS_AT(i)       (poseDpos[(i)])
#define POSENORMAL_AT(i)    (poseDnormal[(i)])
#define BONE_AT(i)          (boneMatrices[(i)])
#define WARPSTATE_LEN       (warpState.length())
#define POSEPOS_LEN         (poseDpos.length())
#define POSEWEIGHTS_LEN     (poseWeights.length())
#define POSENORMAL_LEN      (poseDnormal.length())
#define BONE_LEN            (boneMatrices.length())
#endif

#ifndef CANE_VERTEX_UBO_FALLBACK
// The writable per-vertex lighting cache has no fallback: under §8.1 the
// TRLK_Baked path recomputes the bake every draw instead (see StartThreeWay).
layout(std430, binding = 7) buffer CACHEDLIGHTING
{
    MATERIAL cachedMaterial[];
};
#endif

uniform int fPose;
uniform int poseCount;

uniform int   rko;
uniform int   fAnimateUv;
uniform vec2  uvOffsets;
uniform float unSelfIllum;

uniform int fSkin;
uniform vec3  subGlobPosCenter;
uniform float subGlobRadius;

vec3 normalLocal;
vec3 normalWorld;
vec2 uvLocal;

float objectShadow;
float objectMidtone;
vec3  light;

MATERIAL baseMaterial;

// ES path: Adreno's compiler rejects this interface with explicit locations
// ("output vShadowIndices location or component conflict"), so on GL_ES the
// varyings link by name and the MATERIAL struct travels as three plain outputs
// (copied from the working global at the end of main). Desktop is unchanged.
#ifdef GL_ES
#define V_LOC(n)
#else
#define V_LOC(n) layout(location = n)
#endif

V_LOC(0) out vec4 worldPos;
V_LOC(1) out vec3 worldNormal;
V_LOC(2) out vec4 vertexColor;
V_LOC(3) out vec2 texcoord;

#ifdef GL_ES
MATERIAL material;
out float vMatAmbient;
out vec4  vMatMidtone;
out vec4  vMatLight;
#else
layout(location = 4) out MATERIAL material;
#endif
V_LOC(7) flat out int vShadowCount;
V_LOC(8) flat out ivec4 vShadowIndices[3];
V_LOC(11) out float fogIntensity;

void ApplyWarp(inout vec4 vLocal, inout vec2 uvLocal);
void ApplyPoses(inout vec4 positionLocal, inout vec3 normalLocal);
void StartThreeWay();
void InitGlobLighting();
void ApplyStaticLightsRelight();
void ApplyStaticLights();
void ApplyStaticLightsPass(bool useList);
void ApplyDynamicLightsPass(bool useList);
void ApplyQuickLights();
void ApplyDynamicLights();
void AddDynamicMaterial();
vec4 AddDirectionLight(LIGHT dirlight);
vec4 AddDynamicLight(vec4 dir, vec4 color, vec4 ru, vec4 du);
bool SphereIntersectsPositionLight(LIGHT L, vec3 center, float radius);
vec4 AddPositionLight(LIGHT pointlight);
vec4 AddPositionLightDynamic(LIGHT pointlight);
bool SphereIntersectsFrustum(LIGHT L, vec3 center, float radius);
vec4 AddFrustrumLight(LIGHT frustumlight);
vec4 AddFrustrumLightDynamic(LIGHT frustumlight);
bool FShadowValid(SHADOW shadow, int grfglob);
bool ShadowIntersectsSphere(SHADOW shadow, vec3 objectCenter, float objectRadius);
void CacheWriteMaterial();
void CacheReadMaterial();
void ProcessGlobLighting();
void CalculateFog();
void CalculateFogPS2();
void CalculateFogPS3();

void SetObjectShadowIndex(int slot, int value)
{
    vShadowIndices[slot >> 2][slot & 3] = value;
}

vec3 SafeNormalize(vec3 value)
{
    float length2 = dot(value, value);
    return length2 > 1e-12 ? value * inversesqrt(length2) : vec3(0.0);
}

int AvailableLightCount()
{
#ifdef CANE_VERTEX_UBO_FALLBACK
    return clamp(numLights, 0, MAX_LIGHTS);
#else
    return min(clamp(numLights, 0, MAX_LIGHTS), lights.length());
#endif
}


void main()
{
    worldPos = vec4(0.0);
    worldNormal = vec3(0.0);
    vertexColor = vec4(0.0);
    texcoord = vec2(0.0);
    material.ambient = 0.0;
    material.midtone = vec4(0.0);
    material.light = vec4(0.0);
    vShadowCount = 0;

    for (int i = 0; i < 3; ++i)
        vShadowIndices[i] = ivec4(-1);
    fogIntensity = 0.0;

    vec4 positionLocal = vec4(vertex, 1.0);
    normalLocal = normal;

    if (fPose != 0)
        ApplyPoses(positionLocal, normalLocal);

    if (fSkin != 0 && !CANE_VERT_OFF(4))
    {
        mat4 matSkin = mat4(0.0);
        float totalWeight = 0.0;

        int boneCount = BONE_LEN;

        if (boneWeights.x != 0.0 && boneIndices.x < uint(boneCount))
        {
            matSkin += BONE_AT(boneIndices.x) * boneWeights.x;
            totalWeight += boneWeights.x;
        }

        if (boneWeights.y != 0.0 && boneIndices.y < uint(boneCount))
        {
            matSkin += BONE_AT(boneIndices.y) * boneWeights.y;
            totalWeight += boneWeights.y;
        }

        if (boneWeights.z != 0.0 && boneIndices.z < uint(boneCount))
        {
            matSkin += BONE_AT(boneIndices.z) * boneWeights.z;
            totalWeight += boneWeights.z;
        }

        if (boneWeights.w != 0.0 && boneIndices.w < uint(boneCount))
        {
            matSkin += BONE_AT(boneIndices.w) * boneWeights.w;
            totalWeight += boneWeights.w;
        }

        if (totalWeight > 0.0)
        {
            positionLocal = matSkin * positionLocal;

            vec3 skinnedNormal = mat3(matSkin) * normalLocal;
            float normalLength2 = dot(skinnedNormal, skinnedNormal);

            normalLocal = normalLength2 > 1e-8 ? skinnedNormal * inversesqrt(normalLength2) : vec3(0.0);
        }
    }

    worldPos = op.model * positionLocal;
    worldNormal = SafeNormalize(mat3(op.model) * normalLocal);
    uvLocal = uv;

    if (fAnimateUv > 0)
        uvLocal += uvOffsets;

    if (op.warpType != WARP_NONE)
        ApplyWarp(worldPos, uvLocal);

    vertexColor = color;
    texcoord = uvLocal;

    if (rko == RKO_ThreeWay && !CANE_VERT_OFF(2))
        StartThreeWay();
#ifdef CANE_DIAG_SHADER
    if (caneRelightMismatch)
    {
        material.ambient = 0.0;
        material.midtone = caneBakeWasZero ? vec4(0.0, 1.0, 1.0, 0.0) : vec4(1.0, 0.0, 1.0, 0.0);
        material.light = vec4(0.0);
    }
#endif

    if (numActiveShadows > 0 && !CANE_VERT_OFF(1))
    {
        int shadowCount = clamp(numActiveShadows, 0, MAX_SHADOWS);
        int levelShadowCount = min(max(numLevelShadows, 0), shadows.length());

        for (int i = 0; i < shadowCount; ++i)
        {
            int shIdx = SHADOW_IDX(i);
            if (shIdx < 0 || shIdx >= levelShadowCount)
                continue;
            SHADOW shadow = shadows[shIdx];

            if (!FShadowValid(shadow, op.grfglob))
                continue;

            if (!ShadowIntersectsSphere(shadow, subGlobPosCenter, subGlobRadius))
                continue;

            if (vShadowCount < MAX_OBJECT_SHADOWS)
            {
                SetObjectShadowIndex(vShadowCount, shIdx);
                vShadowCount++;
            }
        }
    }

    if (swp.fogType != FOG_NONE)
        CalculateFog();

#ifdef GL_ES
    vMatAmbient = material.ambient;
    vMatMidtone = material.midtone;
    vMatLight   = material.light;
#endif

    gl_Position = cm.matWorldToClip * worldPos;
}

void ApplyWarp(inout vec4 vLocal, inout vec2 uvLocal)
{
    int matrixCount = clamp(op.warpCmat, 0, 4);
    if (op.warpCvtx <= 0)
        return;

    switch(op.warpType)
    {
        case WARP_POS:
        vec4 sumPos0 = vec4(0.0);

        for (int imat = 0; imat < matrixCount; ++imat)
        {
            int stateIndex = imat * op.warpCvtx + gl_VertexID;
            if (stateIndex < 0 || stateIndex >= WARPSTATE_LEN)
                continue;
            vec4 st  = WARPSTATE_AT(stateIndex);
            sumPos0 += op.amatDpos[imat] * st;
        }

        vLocal.xyz += sumPos0.xyz;
        break;

        case WARP_UV:
        vec2 sumUv0 = vec2(0.0);

        for (int imat = 0; imat < matrixCount; ++imat)
        {
             int stateIndex = imat * op.warpCvtx + gl_VertexID;
             if (stateIndex < 0 || stateIndex >= WARPSTATE_LEN)
                 continue;
             vec4 st = WARPSTATE_AT(stateIndex);
             sumUv0 += (op.amatDuv[imat] * st).xy;
        }

        uvLocal += sumUv0;
        break;

        case WARP_BOTH:
        vec4 sumPos1 = vec4(0.0);
        vec2 sumUv1  = vec2(0.0);

        for (int imat = 0; imat < matrixCount; ++imat)
        {
             int stateIndex = imat * op.warpCvtx + gl_VertexID;
             if (stateIndex < 0 || stateIndex >= WARPSTATE_LEN)
                 continue;
             vec4 st  = WARPSTATE_AT(stateIndex);
             sumPos1 += op.amatDpos[imat] * st;
             sumUv1  += (op.amatDuv[imat] * st).xy;
        }

        vLocal.xyz += sumPos1.xyz;
        uvLocal    += sumUv1;
        break;
    }
}

void ApplyPoses(inout vec4 positionLocal, inout vec3 normalLocal)
{
    vec3 dpos = vec3(0.0);
    vec3 dnormal = vec3(0.0);
    int count = min(max(poseCount, 0), POSEWEIGHTS_LEN);
    int poseBase = gl_VertexID * poseCount;

    for (int ipose = 0; ipose < count; ++ipose)
    {
        float weight = poseWeights[ipose];

        if (weight == 0.0)
            continue;

        int poseIndex = poseBase + ipose;
        if (poseIndex < 0 || poseIndex >= POSEPOS_LEN ||
            poseIndex >= POSENORMAL_LEN)
            continue;

        dpos += POSEPOS_AT(poseIndex).xyz * weight;
        dnormal += POSENORMAL_AT(poseIndex).xyz * weight;
    }

    positionLocal.xyz += dpos;
    normalLocal += dnormal;

    float normalLength2 = dot(normalLocal, normalLocal);

    if (normalLength2 > 1e-8)
        normalLocal *= inversesqrt(normalLength2);
}

void StartThreeWay()
{
    // A transform-feedback bake always runs the relight path, whatever the draw record says.
    int trlkEff = op.trlk;
#ifdef CANE_VERTEX_UBO_FALLBACK
    if (CANE_BAKING) trlkEff = TRLK_Relight;
#endif
    switch(trlkEff)
    {
        // TV portraits use the retail pglob->pblot Quick-light path.  The
        // sole detached TV light is evaluated after pose/skin transforms and
        // the resulting three-way material is baked per vertex for this draw.
        case TRLK_Quick:
        InitGlobLighting();
        ApplyQuickLights();
        ProcessGlobLighting();
        break;

        // Static bake
        case TRLK_Relight:
        InitGlobLighting();
        ApplyStaticLightsRelight();
        ProcessGlobLighting();
        // Bake static lights
        CacheWriteMaterial();
        // Show dynamic this frame (not while baking: the cache holds the static part only)
        if (op.fDynamicLight == 1 && !CANE_BAKING)
            AddDynamicMaterial();
        break;

        case TRLK_Baked:
#ifdef CANE_VERTEX_UBO_FALLBACK
        // §8.1: there is no vertex-stage-writable cache in the fallback, so the static bake is captured
        // by a transform-feedback pass when the glob is relit (DrawGlob) and read back here as
        // attributes; dynamic lights are added on top as before.
        {
            MATERIAL bakedMaterial;
            bakedMaterial.ambient = bakeAmbient;
            bakedMaterial.midtone = bakeMidtone;
            bakedMaterial.light = bakeLight;
#ifdef CANE_DIAG_SHADER
            if (CANE_VERT_OFF(32))
            {
                // Debug self-check: recompute the static bake here and compare it with the cached one.
                InitGlobLighting();
                ApplyStaticLightsRelight();
                ProcessGlobLighting();
                if (abs(material.ambient - bakedMaterial.ambient) > 2e-3 ||
                    any(greaterThan(abs(material.midtone - bakedMaterial.midtone), vec4(2e-3))) ||
                    any(greaterThan(abs(material.light - bakedMaterial.light), vec4(2e-3))))
                {
                    caneRelightMismatch = true;
                    // cyan instead of magenta: the cache holds nothing at all (never baked)
                    if (bakedMaterial.ambient == 0.0 && all(equal(bakedMaterial.midtone, vec4(0.0))) && all(equal(bakedMaterial.light, vec4(0.0))))
                        caneBakeWasZero = true;
                }
            }
#endif
#ifdef CANE_DIAG_SHADER
            if (CANE_VERT_OFF(64))
            {
                // Experiment: ignore the cached attributes and light fresh, like a relight draw.
                InitGlobLighting();
                ApplyStaticLightsRelight();
                ProcessGlobLighting();
            }
            else
#endif
            material = bakedMaterial;
        }
#else
        // Read cached static
        CacheReadMaterial();
#endif
        // Dynamic on top (PS2 DownloadRelight idea)
        if (op.fDynamicLight == 1 && !CANE_VERT_OFF(16))
            AddDynamicMaterial();
        break;

        case TRLK_Dynamic:
        default:
        // Full relight (no cache)
        InitGlobLighting();
        ApplyStaticLights();
        ApplyDynamicLights();
        ProcessGlobLighting();
        break;
    };
}

void ApplyQuickLights()
{
    // Retail sends TRLK_Quick through the same VU lighting path as
    // TRLK_Relight. The TV glob uses its one directly indexed detached light.
    int idx = op.blotTvLight - 1;
    if (idx < 0 || idx >= tvLights.length())
        return;

    LIGHT tvLight = tvLights[idx];
    switch (tvLight.lightk)
    {
        case LIGHTK_Direction:
        light.rgb += AddDirectionLight(tvLight).rgb;
        break;

        case LIGHTK_Position:
        light.rgb += AddPositionLight(tvLight).rgb;
        break;

        case LIGHTK_Frustrum:
        case LIGHTK_Spot:
        light.rgb += AddFrustrumLight(tvLight).rgb;
        break;
    }
}

void InitGlobLighting()
{
    objectShadow = swp.uShadow;
    objectMidtone = swp.uMidtone + unSelfIllum * 0.000031;
    light = vec3(0.0);
    normalWorld = worldNormal;
}

void RelightAddOne(int i)
{
    LIGHT L = LIGHTAT(i);
    if (L.fActive == 0 || L.fDynamic != 0)
        return;

    switch (L.lightk)
    {
        case LIGHTK_Direction:
        light.rgb += AddDirectionLight(L).rgb;
        break;

        case LIGHTK_Position:
        if (!SphereIntersectsPositionLight(L, op.posCenter.xyz, op.sRadius))
            return;

        light.rgb += AddPositionLight(L).rgb;
        break;

        case LIGHTK_Frustrum:
        case LIGHTK_Spot:
        if (!SphereIntersectsFrustum(L, op.posCenter.xyz, op.sRadius))
            return;

        light.rgb += AddFrustrumLight(L).rgb;
        break;
    }
}

void ApplyStaticLightsRelight()
{
    // A relight result is cached on the glob and can outlive the camera's
    // current BSP zone.  Build that persistent cache from the complete GPU
    // light table instead of the camera-zone-filtered active-light list.
    int lightCount = AvailableLightCount();

#ifdef CANE_VERTEX_UBO_FALLBACK
#ifdef CANE_DIAG_SHADER
    // Debug self-check (debug.cane.vert bit 32): compute both ways and flag any difference.
    if (CANE_VERT_OFF(32) && op.relightHdr.x > 0 && op.relightHdr.z == 0)
    {
        vec3 saved = light;
        int listCount0 = min(op.relightHdr.x - 1, 16);
        for (int k = 0; k < listCount0; ++k)
        {
            int i = op.relightIdx[k >> 2][k & 3];
            if (i >= 0 && i < lightCount)
                RelightAddOne(i);
        }
        vec3 viaList = light;
        light = saved;
        for (int i = 0; i < lightCount; ++i)
            RelightAddOne(i);
        vec3 viaTable = light;
        if (any(greaterThan(abs(viaList - viaTable), vec3(1e-4))))
            caneRelightMismatch = true;
        return;
    }
#endif
    // Fallback: the CPU supplies the (few) lights that can reach this draw; the exact
    // per-light tests below still run on each one, so the result is identical.
    if (op.relightHdr.x > 0 && op.relightHdr.z == 0)
    {
        int listCount = min(op.relightHdr.x - 1, 16);
        for (int k = 0; k < listCount; ++k)
        {
            int i = op.relightIdx[k >> 2][k & 3];
            if (i >= 0 && i < lightCount)
                RelightAddOne(i);
        }
        return;
    }
#endif

    for (int i = 0; i < lightCount; ++i)
        RelightAddOne(i);
}

void ApplyStaticLightsPass(bool useList)
{
    if (numStaticLights == 0)
        return;

    if (op.fDynamic != 1)
    {
        int count = useList ? min(op.relightHdr.x - 1, 16) : clamp(numStaticLights, 0, MAX_LIGHTS);
        for (int i = 0; i < count; ++i)
        {
            int idx = useList ? op.relightIdx[i >> 2][i & 3] : STATIC_LIGHT_IDX(i);
            if (idx < 0 || idx >= AvailableLightCount())
                continue;

            LIGHT Lc = LIGHTAT(idx);

            switch (Lc.lightk)
            {
                case LIGHTK_Direction:
                light.rgb += AddDirectionLight(Lc).rgb;
                break;

                case LIGHTK_Position:
                if (!SphereIntersectsPositionLight(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddPositionLight(Lc).rgb;
                break;

                case LIGHTK_Frustrum:
                case LIGHTK_Spot:
                if (!SphereIntersectsFrustum(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddFrustrumLight(Lc).rgb;
                break;
            }
        }
    }
    else
    {
        int count = useList ? min(op.relightHdr.x - 1, 16) : clamp(numStaticLights, 0, MAX_LIGHTS);
        for (int i = 0; i < count; ++i)
        {
            int idx = useList ? op.relightIdx[i >> 2][i & 3] : STATIC_LIGHT_IDX(i);
            if (idx < 0 || idx >= AvailableLightCount())
                continue;

            LIGHT Lc = LIGHTAT(idx);

            if (Lc.fExcludeDynamicObjects != 0)
                continue;

            switch (Lc.lightk)
            {
                case LIGHTK_Direction:
                light.rgb += AddDynamicLight(Lc.dir, Lc.color, Lc.ru, Lc.du).rgb;
                break;

                case LIGHTK_Position:
                if (!SphereIntersectsPositionLight(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddPositionLightDynamic(Lc).rgb;
                break;

                case LIGHTK_Frustrum:
                case LIGHTK_Spot:
                if (!SphereIntersectsFrustum(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddFrustrumLightDynamic(Lc).rgb;
                break;
            }
        }
    }
}

void ApplyDynamicLightsPass(bool useList)
{
    if (numDynamicLights == 0)
        return;

    if (op.fDynamic != 1)
    {
        int count = useList ? min(op.relightHdr.y - 1, 12) : clamp(numDynamicLights, 0, MAX_LIGHTS);
        for (int i = 0; i < count; ++i)
        {
            int idx = useList ? op.relightIdx[4 + (i >> 2)][i & 3] : DYN_LIGHT_IDX(i);
            if (idx < 0 || idx >= AvailableLightCount())
                continue;

            LIGHT Lc = LIGHTAT(idx);

            switch (Lc.lightk)
            {
                case LIGHTK_Direction:
                light.rgb += AddDirectionLight(Lc).rgb;
                break;

                case LIGHTK_Position:
                if (!SphereIntersectsPositionLight(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddPositionLight(Lc).rgb;
                break;

                case LIGHTK_Frustrum:
                case LIGHTK_Spot:
                if (!SphereIntersectsFrustum(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddFrustrumLight(Lc).rgb;
                break;
            }
        }
    }
    else
    {
        int count = useList ? min(op.relightHdr.y - 1, 12) : clamp(numDynamicLights, 0, MAX_LIGHTS);
        for (int i = 0; i < count; ++i)
        {
            int idx = useList ? op.relightIdx[4 + (i >> 2)][i & 3] : DYN_LIGHT_IDX(i);
            if (idx < 0 || idx >= AvailableLightCount())
                continue;

            LIGHT Lc = LIGHTAT(idx);

            if (Lc.fExcludeDynamicObjects != 0)
                continue;

            switch (Lc.lightk)
            {
                case LIGHTK_Direction:
                light.rgb += AddDynamicLight(Lc.dir, Lc.color, Lc.ru, Lc.du).rgb;
                break;

                case LIGHTK_Position:
                if (!SphereIntersectsPositionLight(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddPositionLightDynamic(Lc).rgb;
                break;

                case LIGHTK_Frustrum:
                case LIGHTK_Spot:
                if (!SphereIntersectsFrustum(Lc, op.posCenter.xyz, op.sRadius))
                    continue;

                light.rgb += AddFrustrumLightDynamic(Lc).rgb;
                break;
            }
        }
    }
}

void ApplyStaticLights()
{
    bool useList = op.relightHdr.x > 0 && op.relightHdr.z == 1;
#ifdef CANE_DIAG_SHADER
    if (CANE_VERT_OFF(32) && useList)
    {
        float s0 = objectShadow, m0 = objectMidtone; vec3 l0 = light;
        ApplyStaticLightsPass(true);
        vec3 a = vec3(objectShadow, objectMidtone, 0.0) + light;
        objectShadow = s0; objectMidtone = m0; light = l0;
        ApplyStaticLightsPass(false);
        vec3 b = vec3(objectShadow, objectMidtone, 0.0) + light;
        if (any(greaterThan(abs(a - b), vec3(1e-4))))
            caneRelightMismatch = true;
        return;
    }
#endif
    ApplyStaticLightsPass(useList);
}

void ApplyDynamicLights()
{
    bool useList = op.relightHdr.y > 0;
#ifdef CANE_DIAG_SHADER
    if (CANE_VERT_OFF(32) && useList)
    {
        float s0 = objectShadow, m0 = objectMidtone; vec3 l0 = light;
        ApplyDynamicLightsPass(true);
        vec3 a = vec3(objectShadow, objectMidtone, 0.0) + light;
        objectShadow = s0; objectMidtone = m0; light = l0;
        ApplyDynamicLightsPass(false);
        vec3 b = vec3(objectShadow, objectMidtone, 0.0) + light;
        if (any(greaterThan(abs(a - b), vec3(1e-4))))
            caneRelightMismatch = true;
        return;
    }
#endif
    ApplyDynamicLightsPass(useList);
}

void AddDynamicMaterial()
{
    baseMaterial = material;

    objectShadow = 0.0;
    objectMidtone = 0.0;
    light = vec3(0.0);
    normalWorld = worldNormal;

    ApplyDynamicLights();
    ProcessGlobLighting();

    material.ambient = baseMaterial.ambient + material.ambient;
    material.midtone = baseMaterial.midtone + material.midtone;
    material.light = baseMaterial.light + material.light;
}

vec4 AddDirectionLight(LIGHT dirlight)
{
    vec3 L = SafeNormalize(dirlight.dir.xyz);

    float f = dot(normalWorld, L);

    // PS2 curve: dot + dot^3
    f = f + f * f * f;

    float shadow    = max(f * dirlight.ru.x + dirlight.du.x, 0.0);
    float midtone   = max(f * dirlight.ru.y + dirlight.du.y, 0.0);
    float highlight = max(f * dirlight.ru.z + dirlight.du.z, 0.0);

    objectShadow  += shadow;
    objectMidtone += midtone;

    return vec4(dirlight.color * highlight);
}

vec4 AddDynamicLight(vec4 dir, vec4 color, vec4 ru, vec4 du)
{
    vec3 lightDir = SafeNormalize(mat3(transpose(op.model)) * vec3(dir));
    float diffuse = dot(lightDir, normalLocal);
    diffuse += diffuse * diffuse * diffuse;

    float shadow    = diffuse * ru.x + du.x;
    float midtone   = diffuse * ru.y + du.y;
    float highlight = diffuse * ru.z + du.z;

    objectShadow  += max(shadow,  0.0);
    objectMidtone += max(midtone, 0.0);
    highlight      = max(highlight, 0.0);

    return color * highlight;
}

bool SphereIntersectsPositionLight(LIGHT L, vec3 center, float radius)
{
    // Vector from light to object center
    vec3 toLight = L.pos.xyz - center;

    // Squared distance between centers
    float distSq = dot(toLight, toLight);

    // Position-light influence radius:
    // L.dst = light falloff (gMax), same as PS2 lmFallOffS.gMax
    float maxDist = L.dst + radius;   // linear units

    // Compare using squared distance
    return distSq <= maxDist * maxDist;
}

vec4 AddPositionLight(LIGHT pointlight)
{
    vec3 toLight = pointlight.pos.xyz - worldPos.xyz;

    float distance2 = dot(toLight, toLight);
    if (distance2 <= 1e-12)
        return vec4(0.0);

    float invLen = inversesqrt(distance2);
    vec3 L = toLight * invLen;

    vec3 N = normalWorld;
    float n2 = dot(N, N);

    if (n2 >= 0.00000001)
        N *= inversesqrt(n2);
    else
        N = vec3(0.0);

    float ndotl = dot(N, L);

    float diffuse = ndotl + ndotl * ndotl * ndotl;

    float attenuation = clamp(pointlight.constant + pointlight.invDst * invLen, 0.0, 1.0);

    float shadow    = max(diffuse * pointlight.ru.x + pointlight.du.x, 0.0) * attenuation;
    float midtone   = max(diffuse * pointlight.ru.y + pointlight.du.y, 0.0) * attenuation;
    float highlight = max(diffuse * pointlight.ru.z + pointlight.du.z, 0.0) * attenuation;

    objectShadow  += shadow;
    objectMidtone += midtone;

    return vec4(pointlight.color * highlight);
}

vec4 AddPositionLightDynamic(LIGHT pointlight)
{
    vec3 direction = pointlight.pos.xyz - op.posCenter.xyz;
    float distanceSquared = dot(direction, direction);

    if (distanceSquared <= 0.00000001)
        return vec4(0.0);

    float inverseDistance = inversesqrt(distanceSquared);
    float attenuation =
        clamp(pointlight.constant + pointlight.invDst * inverseDistance, 0.0, 1.0);

    return AddDynamicLight(
        vec4(direction, 0.0),
        pointlight.color,
        pointlight.ru * attenuation,
        pointlight.du * attenuation);
}

bool SphereIntersectsFrustum(LIGHT L, vec3 center, float radius)
{
    mat4 M = L.matFrustrum;

    // GLSL indexes matrices by column.  Frustum planes are combinations of
    // matrix rows, so construct the rows explicitly. PS2 VU CLIP uses
    // symmetric -w..w clipping for Z as well as X and Y.
    vec4 row0 = vec4(M[0][0], M[1][0], M[2][0], M[3][0]);
    vec4 row1 = vec4(M[0][1], M[1][1], M[2][1], M[3][1]);
    vec4 row2 = vec4(M[0][2], M[1][2], M[2][2], M[3][2]);
    vec4 row3 = vec4(M[0][3], M[1][3], M[2][3], M[3][3]);

    vec4 planes[6];
    planes[0] = row3 + row0;
    planes[1] = row3 - row0;
    planes[2] = row3 + row1;
    planes[3] = row3 - row1;
    planes[4] = row3 + row2;
    planes[5] = row3 - row2;

    for (int i = 0; i < 6; ++i)
    {
        vec3 n = planes[i].xyz;
        float len = length(n);

        if (len < 1e-6) continue;

        n /= len;
        float d = planes[i].w / len;

        float dist = dot(n, center) + d;
        if (dist < -radius)
            return false;
    }

    return true;
}

vec4 AddFrustrumLight(LIGHT frustumlight)
{
    vec4 clipL = frustumlight.matFrustrum * vec4(worldPos.xyz, 1.0);

    if (clipL.w <= 0.0) return vec4(0.0);
    if (abs(clipL.x) > clipL.w) return vec4(0.0);
    if (abs(clipL.y) > clipL.w) return vec4(0.0);
    if (abs(clipL.z) > clipL.w) return vec4(0.0);

    float invW = 1.0 / clipL.w;

    float u  = abs(clipL.x * invW);
    float v  = abs(clipL.y * invW);
    float r2 = u * u + v * v;
    float wv = clipL.w;

    float fx = clamp(frustumlight.falloffScale.x * u  + frustumlight.falloffBias.x, 0.0, 1.0);
    float fy = clamp(frustumlight.falloffScale.y * v  + frustumlight.falloffBias.y, 0.0, 1.0);
    float fr = clamp(frustumlight.falloffScale.z * r2 + frustumlight.falloffBias.z, 0.0, 1.0);
    float fw = clamp(frustumlight.falloffScale.w * wv + frustumlight.falloffBias.w, 0.0, 1.0);

    float mask = fx * fy * fr * fw;
    if (mask <= 0.0) return vec4(0.0);

    return AddDynamicLight(frustumlight.dir, frustumlight.color, frustumlight.ru * mask, frustumlight.du * mask);
}

vec4 AddFrustrumLightDynamic(LIGHT frustumlight)
{
    vec4 clipL = frustumlight.matFrustrum * vec4(op.posCenter.xyz, 1.0);

    if (clipL.w <= 0.0) return vec4(0.0);
    if (abs(clipL.x) > clipL.w) return vec4(0.0);
    if (abs(clipL.y) > clipL.w) return vec4(0.0);
    if (abs(clipL.z) > clipL.w) return vec4(0.0);

    float invW = 1.0 / clipL.w;

    float u  = abs(clipL.x * invW);
    float v  = abs(clipL.y * invW);
    float r2 = u * u + v * v;
    float wv = clipL.w;

    float fx = clamp(frustumlight.falloffScale.x * u  + frustumlight.falloffBias.x, 0.0, 1.0);
    float fy = clamp(frustumlight.falloffScale.y * v  + frustumlight.falloffBias.y, 0.0, 1.0);
    float fr = clamp(frustumlight.falloffScale.z * r2 + frustumlight.falloffBias.z, 0.0, 1.0);
    float fw = clamp(frustumlight.falloffScale.w * wv + frustumlight.falloffBias.w, 0.0, 1.0);

    float mask = fx * fy * fr * fw;
    if (mask <= 0.0) return vec4(0.0);

    vec4 ruScaled = frustumlight.ru * mask;
    vec4 duScaled = frustumlight.du * mask;

    return AddDynamicLight(frustumlight.dir, frustumlight.color, ruScaled, duScaled);
}

bool FShadowValid(SHADOW shadow, int grfglob)
{
    if ((grfglob & 1) != 0 && shadow.shdk == 2)
        return true;

    if ((grfglob & 2) != 0 && shadow.shdk == 3)
        return true;

    return false;
}

bool ShadowIntersectsSphere(SHADOW shadow, vec3 objectCenter, float objectRadius)
{
    if (shadow.fDynamic == 1)
    {
        vec3 N = SafeNormalize(worldNormal);
        vec3 castDir = SafeNormalize(shadow.normalCast.xyz);

        if (dot(N, -castDir) <= -0.5)
            return false;
    }

    vec3  d = objectCenter - vec3(shadow.posEffect);
    float r = objectRadius + shadow.sRadiusEffect;

    return dot(d, d) <= r * r;
}

void CacheWriteMaterial()
{
#ifndef CANE_VERTEX_UBO_FALLBACK
    if (gl_VertexID < 0 || gl_VertexID >= cachedMaterial.length())
        return;

    cachedMaterial[gl_VertexID].ambient = material.ambient;
    cachedMaterial[gl_VertexID].midtone = material.midtone;
    cachedMaterial[gl_VertexID].light   = material.light;
#endif
}

void CacheReadMaterial()
{
#ifndef CANE_VERTEX_UBO_FALLBACK
    if (gl_VertexID < 0 || gl_VertexID >= cachedMaterial.length())
    {
        material.ambient = 0.0;
        material.midtone = vec4(0.0);
        material.light = vec4(0.0);
        return;
    }

    material.ambient = cachedMaterial[gl_VertexID].ambient;
    material.midtone = cachedMaterial[gl_VertexID].midtone;
    material.light   = cachedMaterial[gl_VertexID].light;
#endif
}

void ProcessGlobLighting()
{
    // Find the brightest channel in the light color (used to gauge overall intensity)
    float dominantLight = max(max(light.r, light.g), light.b);
    // Invert brightness to get potential shadow strength (brighter light = less shadow)
    float shadowModifier = 1.0 - dominantLight;
    // Clamp the object's midtone value so it doesn't exceed remaining shadow range
    float clampedMidtone = clamp(objectMidtone, 0.0, shadowModifier);
    // Determine how much of the remaining light can go toward shadows (bounded by objectShadow)
    float shadowContribution = max(min(shadowModifier - clampedMidtone, objectShadow), 0.0);
    // Approximate base light intensity from average of vertex color (grayscale luminance)
    float baseIntensity = dot(vertexColor.rgb, vec3(0.3333333));

    // Assign lighting output
    material.ambient   = shadowContribution * baseIntensity;
    material.midtone = vec4(clampedMidtone * vertexColor.rgb, 0.0);
    material.light = vec4(min(light.rgb, vec3(1.0)) * baseIntensity, 0.0);
}

void CalculateFog()
{
    switch(swp.fogType)
    {
        case FOG_PS2:
        CalculateFogPS2();
        break;

        case FOG_PS3:
        CalculateFogPS3();
        break;
    }
}

void CalculateFogPS2()
{
    float z = length(cm.cameraPos.xyz - worldPos.xyz);
    float recipZ = 1.0 / max(z, 1e-4);

    float recipNear = 1.0 / max(abs(swp.fogNear), 1e-6);
    float recipFar  = 1.0 / max(abs(swp.fogFar), 1e-6);

    float denom = max(recipNear - recipFar, 1e-6);
    float fog   = clamp((recipNear - recipZ) * (1.0 / denom), 0.0, 1.0);

    float fogMult = mix(swp.fogMax, swp.fogMax * op.uFog, step(0.001, op.uFog));
    fogIntensity  = clamp((fog * fogMult) * swp.fogColor.a, 0.0, 1.0);
}

void CalculateFogPS3()
{
    vec3 offset = cm.cameraPos.xyz - worldPos.xyz;

    float distance2 = dot(offset, offset);
    float distanceToCamera = sqrt(distance2);

    float invFogRange = 1.0 / max(swp.fogFar - swp.fogNear, 1e-6);
    float fog = clamp((distanceToCamera - swp.fogNear) * invFogRange, 0.0, 1.0);

    float fogMult = mix(swp.fogMax, swp.fogMax * op.uFog, step(0.001, op.uFog));
    fogIntensity  = clamp((fog * fogMult) * swp.fogColor.a, 0.0, 1.0);
}
