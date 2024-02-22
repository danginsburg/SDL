SamplerState theSampler : register(s0);
Texture2D theTexture : register(t1);

#include "VULKAN_PixelShader_Common.incl"

float4 main(PixelShaderInput input) : SV_TARGET
{
    return GetOutputColor(theTexture.Sample(theSampler, input.tex)) * input.color;
}
