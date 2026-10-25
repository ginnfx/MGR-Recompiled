#include "copy_common.hlsli"

Texture2D<float> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    uint w, h;
    g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex].GetDimensions(w, h);
    uint origin = g_PushConstants.ResourceDescriptorIndex2;
    texCoord = texCoord * float2(g_PushConstants.Param0, g_PushConstants.Param1)
        + float2(origin & 65535u, origin >> 16) / float2(w, h);
    // Integer fetch preserves the original depth samples at atlas tile edges.
    int2 coord = clamp(int2(texCoord * float2(w, h)), int2(0, 0), int2(w - 1, h - 1));
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex].Load(int3(coord, 0));
}
