#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// Contrast adaptive sharpening after AMD FidelityFX CAS (MIT): a cross of
// source texels, weighted down where local contrast is already high.
// sharpness 0..1; 0 returns the centre sample unchanged.
float3 Sharpen(Texture2D<float4> tex, float2 uv, float3 centre, float sharpness)
{
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 texel = 1.0 / float2(w, h);
    SamplerState s = g_SamplerDescriptorHeap[0];
    float3 n = tex.Sample(s, uv - float2(0.0, texel.y)).rgb;
    float3 so = tex.Sample(s, uv + float2(0.0, texel.y)).rgb;
    float3 e = tex.Sample(s, uv + float2(texel.x, 0.0)).rgb;
    float3 we = tex.Sample(s, uv - float2(texel.x, 0.0)).rgb;
    float3 mn = min(centre, min(min(n, so), min(e, we)));
    float3 mx = max(centre, max(max(n, so), max(e, we)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 weight = -amp / lerp(8.0, 5.0, sharpness);
    return saturate((centre + weight * (n + so + e + we)) / (1.0 + 4.0 * weight));
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float3 rgb = tex.Sample(g_SamplerDescriptorHeap[0], texCoord).rgb;
    // Param1 = sharpness. Scanout stays uncorrected: Xenia measurably shows the
    // raw front buffer (its DC LUT stays linear), not the TV ramp.
    if (g_PushConstants.Param1 > 0.0)
        rgb = Sharpen(tex, texCoord, rgb, g_PushConstants.Param1);
    // max() guards pow() against NaN on negative HDR values (source RT may be R16F).
    // Param0 = gamma (1/brightness).
    return float4(pow(max(rgb, 0.0), g_PushConstants.Param0), 1.0);
}
