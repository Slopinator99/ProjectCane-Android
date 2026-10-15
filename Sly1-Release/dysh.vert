#version 430 core

layout (location = 0) in vec3  vertex;
layout (location = 1) in vec3  normal;
layout (location = 2) in vec4  color;
layout (location = 3) in vec2  uv;
layout (location = 4) in uvec4 boneIndices;
layout (location = 5) in vec4  boneWeights;

uniform mat4 model;
uniform mat4 matWorldToClip; 
uniform int fSkin;

#ifdef CANE_VERTEX_UBO_FALLBACK
// §8.1 vertex-SSBO fallback: bone matrices are a 4-wide data texture (unit 5,
// fixed in gl.cpp); textureSize().y is the exact matrix count.
uniform sampler2D bonesTex;

#define BONE_AT(i)      mat4(texelFetch(bonesTex, ivec2(0, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(1, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(2, int(i)), 0), \
                             texelFetch(bonesTex, ivec2(3, int(i)), 0))
#define BONE_LEN        (textureSize(bonesTex, 0).y)
#else
layout(std430, binding = 9) readonly buffer BONEBLK
{
    mat4 boneMatrices[];
};

#define BONE_AT(i)      (boneMatrices[(i)])
#define BONE_LEN        (boneMatrices.length())
#endif

layout(location = 0) out vec4 vertexColor;

void main()
{
    vertexColor = color;

    vec4 positionLocal = vec4(vertex, 1.0);

    if (fSkin != 0)
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
            positionLocal = matSkin * positionLocal;
    }

    gl_Position = matWorldToClip * model * positionLocal;
}
