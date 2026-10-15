#version 430 core


layout(std140, binding = 0) uniform CMGL
{
    mat4 matWorldToClip;
    vec4 cameraPos;
} cm;

layout(std140, binding = 1) uniform ROCEL
{
    mat4 model;
    vec4 celRgba;
    float uAlphaCelBorder;
} cbp;

#ifdef CANE_VERTEX_UBO_FALLBACK
// §8.1 vertex-SSBO fallback: the cel-border edge data becomes data textures
// (sampler units fixed in gl.cpp: edges 4, bones 5, indices 6, weights 7,
// pose deltas 8); access macros collapse to the SSBO expressions on the
// desktop path. Edge indices/weights/poses share edges' element count.
#define MAX_POSES 64
uniform sampler2D edgesTex;
uniform sampler2D bonesTex;
uniform highp usampler2D edgeBoneIdxTex;
uniform sampler2D edgeBoneWTex;
uniform sampler2D edgePoseTex;
uniform float poseWeights[MAX_POSES];

#define EDGES_AT(i)     texelFetch(edgesTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define EDGE_IDX_AT(i)  texelFetch(edgeBoneIdxTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define EDGE_W_AT(i)    texelFetch(edgeBoneWTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define EDGE_POSE_AT(i) texelFetch(edgePoseTex, ivec2((i) & 1023, ((i) >> 10)), 0)
#define BONE_AT(i)      mat4(texelFetch(bonesTex, ivec2(0, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(1, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(2, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(3, int(i)), 0))
#define EDGES_LEN       (textureSize(edgesTex, 0).x * textureSize(edgesTex, 0).y)
#define EDGE_POSE_LEN   (textureSize(edgePoseTex, 0).x * textureSize(edgePoseTex, 0).y)
#define BONE_LEN        (textureSize(bonesTex, 0).y)
#define POSEWEIGHTS_LEN MAX_POSES
#else
struct CELPOINT
{
    vec4 position;
    uvec4 boneIndices;
    vec4 boneWeights;
};

layout(std430, binding = 8) readonly buffer EDGEBUFFER
{
    CELPOINT edgePoints[];
};

layout(std430, binding = 9) readonly buffer BONEBLK
{
    mat4 boneMatrices[];
};

layout(std430, binding = 12) readonly buffer EDGEPOSEPOSBLK
{
    vec4 edgePoseDpos[];
};

layout(std430, binding = 14) readonly buffer POSEWEIGHTBLK
{
    float poseWeights[];
};

#define EDGES_AT(i)     (edgePoints[(i)].position)
#define EDGE_IDX_AT(i)  (edgePoints[(i)].boneIndices)
#define EDGE_W_AT(i)    (edgePoints[(i)].boneWeights)
#define EDGE_POSE_AT(i) (edgePoseDpos[(i)])
#define BONE_AT(i)      (boneMatrices[(i)])
#define EDGES_LEN       (edgePoints.length())
#define EDGE_POSE_LEN   (edgePoseDpos.length())
#define BONE_LEN        (boneMatrices.length())
#define POSEWEIGHTS_LEN (poseWeights.length())
#endif

uniform int fSkin;
uniform int fPose;
uniform int poseCount;

void CullVertex();
float Cross2D(vec2 a, vec2 b);
vec3 GetEdgePosition(int pointIndex);

void main()
{
    int edgeID = gl_VertexID >> 1;
    bool first = (gl_VertexID & 1) == 0;
    int pointBase = edgeID * 4;

    if (pointBase < 0 || pointBase + 3 >= EDGES_LEN)
    {
        CullVertex();
        return;
    }

    vec3 E0 = GetEdgePosition(pointBase + 0);
    vec3 E1 = GetEdgePosition(pointBase + 1);
    vec3 OA = GetEdgePosition(pointBase + 2);
    vec3 OB = GetEdgePosition(pointBase + 3);

    mat4 mvp = cm.matWorldToClip * cbp.model;

    vec4 A = mvp * vec4(E0, 1.0);
    vec4 B = mvp * vec4(E1, 1.0);
    vec4 C = mvp * vec4(OA, 1.0);
    vec4 D = mvp * vec4(OB, 1.0);

    if (A.w <= 0.0 || B.w <= 0.0 || C.w <= 0.0 || D.w <= 0.0)
    {
        CullVertex();
        return;
    }

    vec3 a = A.xyz / A.w;
    vec3 b = B.xyz / B.w;
    vec3 c = C.xyz / C.w;
    vec3 d = D.xyz / D.w;

    float cross0 = Cross2D(b.xy - a.xy, c.xy - b.xy);
    float cross1 = Cross2D(d.xy - b.xy, b.xy - a.xy);

    if ((cross0 * cross1) > 0.0)
    {
        CullVertex();
        return;
    }

    gl_Position = first ? A : B;
}

void CullVertex()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}

float Cross2D(vec2 a, vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

vec3 GetEdgePosition(int pointIndex)
{
    vec4 position = EDGES_AT(pointIndex);

    if (fPose != 0)
    {
        int count = min(max(poseCount, 0), POSEWEIGHTS_LEN);
        int poseBase = pointIndex * poseCount;

        for (int ipose = 0; ipose < count; ++ipose)
        {
            float weight = poseWeights[ipose];

            int poseIndex = poseBase + ipose;
            if (weight != 0.0 && poseIndex >= 0 &&
                poseIndex < EDGE_POSE_LEN)
                position.xyz += EDGE_POSE_AT(poseIndex).xyz * weight;
        }
    }

    if (fSkin == 0)
        return position.xyz;

    if (pointIndex < 0 || pointIndex >= EDGES_LEN)
        return position.xyz;

    uvec4 indices = EDGE_IDX_AT(pointIndex);
    vec4 weights = EDGE_W_AT(pointIndex);
    vec4 localPosition = vec4(position.xyz, 1.0);
    vec4 skinnedPosition = vec4(0.0);
    float totalWeight = 0.0;
    int boneCount = BONE_LEN;

    if (weights.x != 0.0 && indices.x < uint(boneCount))
    {
        skinnedPosition += BONE_AT(indices.x) * localPosition * weights.x;
        totalWeight += weights.x;
    }

    if (weights.y != 0.0 && indices.y < uint(boneCount))
    {
        skinnedPosition += BONE_AT(indices.y) * localPosition * weights.y;
        totalWeight += weights.y;
    }

    if (weights.z != 0.0 && indices.z < uint(boneCount))
    {
        skinnedPosition += BONE_AT(indices.z) * localPosition * weights.z;
        totalWeight += weights.z;
    }

    if (weights.w != 0.0 && indices.w < uint(boneCount))
    {
        skinnedPosition += BONE_AT(indices.w) * localPosition * weights.w;
        totalWeight += weights.w;
    }

    return totalWeight > 0.0 ? skinnedPosition.xyz : position.xyz;
}
