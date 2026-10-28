#pragma once
#include "copy_common.hlsli"

// Multisampled source bound at the same bindless slot/space as the non-MS
// heap. plume creates a Texture2DMS SRV automatically when the source
// texture's sampleCount > 1. Declaring the heap as Texture2DMS here reads
// that descriptor correctly (mirrors UR's g_Texture2DMSDescriptorHeap).
Texture2DMS<float4, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

static const uint kMaxBoxTaps = 8u;

// Same mapping as copy_color_ps: Param1 = source pixels per dest pixel (0:
// stretch the whole source), ResourceDescriptorIndex2 = signed 16-bit
// half-pixel source offset.
float2 SourceOffset()
{
    uint packed = g_PushConstants.ResourceDescriptorIndex2;
    return float2(int(packed << 16) >> 16, int(packed) >> 16) * 0.5;
}

float4 LoadPixel(Texture2DMS<float4, SAMPLE_COUNT> tex, int2 coord, int2 size)
{
    coord = clamp(coord, int2(0, 0), size - 1);
    float4 result = tex.Load(coord, 0);
    [unroll] for (int i = 1; i < SAMPLE_COUNT; i++)
        result += tex.Load(coord, i);
    return result / SAMPLE_COUNT;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DMS<float4, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    const int2 size = int2(w, h);
    float ratio = g_PushConstants.Param1;
    uint box = (uint)(ratio + 0.5);
    float4 result;
    if (box >= 2u && abs(ratio - (float)box) < 0.001)
    {
        uint taps = min(box, kMaxBoxTaps);
        int2 base = int2(position.xy) * (int)box + int2(SourceOffset());
        result = 0.0;
        [loop] for (uint y = 0u; y < taps; ++y)
            [loop] for (uint x = 0u; x < taps; ++x)
                result += LoadPixel(tex, base + int2((x * box + box / 2u) / taps,
                                                     (y * box + box / 2u) / taps), size);
        result /= (float)(taps * taps);
    }
    else if (ratio > 0.0)
    {
        result = LoadPixel(tex, int2(floor(position.xy * ratio + SourceOffset())), size);
    }
    else
    {
        result = LoadPixel(tex, int2(texCoord * float2(w, h)), size);
    }
    // Param0 = the X360 resolve exponent-bias scale (2^bias), as copy_color_ps.
    return float4(result.rgb * g_PushConstants.Param0, result.a);
}
