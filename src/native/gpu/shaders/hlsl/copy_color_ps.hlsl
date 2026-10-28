#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

static const uint kMaxBoxTaps = 8u;

// Param1 = source pixels per dest pixel (0: stretch the whole source over the
// target). ResourceDescriptorIndex2 = source offset, two signed 16-bit
// half-pixel counts, so source pixel = dest pixel * Param1 + offset.
float2 SourceOffset()
{
    uint packed = g_PushConstants.ResourceDescriptorIndex2;
    return float2(int(packed << 16) >> 16, int(packed) >> 16) * 0.5;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float ratio = g_PushConstants.Param1;
    uint box = (uint)(ratio + 0.5);
    float4 s;
    if (box >= 2u && abs(ratio - (float)box) < 0.001)
    {
        uint taps = min(box, kMaxBoxTaps);
        int2 base = int2(position.xy) * (int)box + int2(SourceOffset());
        float4 acc = 0.0;
        [loop] for (uint y = 0u; y < taps; ++y)
        {
            int oy = (int)((y * box + box / 2u) / taps);
            [loop] for (uint x = 0u; x < taps; ++x)
            {
                int ox = (int)((x * box + box / 2u) / taps);
                acc += tex.Load(int3(base.x + ox, base.y + oy, 0));
            }
        }
        s = acc / (float)(taps * taps);
    }
    else if (ratio > 0.0)
    {
        uint w, h;
        tex.GetDimensions(w, h);
        s = tex.Sample(g_SamplerDescriptorHeap[0],
                       (position.xy * ratio + SourceOffset()) / float2(w, h));
    }
    else
    {
        s = tex.Sample(g_SamplerDescriptorHeap[0], texCoord);
    }
    return float4(s.rgb * g_PushConstants.Param0, s.a);
}
