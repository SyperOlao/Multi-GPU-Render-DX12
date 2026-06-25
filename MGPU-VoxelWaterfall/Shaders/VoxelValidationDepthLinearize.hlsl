Texture2D<float> PrimaryHardwareDepth : register(t0);
Texture2D<float4> SecondaryColor : register(t1);
Texture2D<float> SecondaryLinearDepth : register(t2);

RWTexture2D<float> OutputLinearDepth : register(u0);

cbuffer DepthLinearizeConstants : register(b0)
{
    uint2 RenderSize;
    float NearZ;
    float FarZ;
    float DepthEpsilon;
    uint UseSecondary;
    float SecondaryInvalidDepth;
    float Padding0;
};

float HardwareDepthToLinearViewDepth(float hardwareDepth)
{
    const float denominator = max(FarZ - hardwareDepth * (FarZ - NearZ), 1.0e-6f);
    return (NearZ * FarZ) / denominator;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= RenderSize.x || dispatchThreadId.y >= RenderSize.y)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float primaryHardwareDepth = PrimaryHardwareDepth.Load(uint3(pixel, 0));
    const float primaryLinearDepth = HardwareDepthToLinearViewDepth(primaryHardwareDepth);

    float outputDepth = primaryLinearDepth;
    if (UseSecondary != 0u)
    {
        const float4 secondaryColor = SecondaryColor.Load(uint3(pixel, 0));
        const float secondaryDepth = SecondaryLinearDepth.Load(uint3(pixel, 0));
        const bool hasSecondary = secondaryColor.a > 0.0f;
        const bool validSecondaryDepth =
            hasSecondary &&
            isfinite(secondaryDepth) &&
            secondaryDepth > 0.0f &&
            secondaryDepth < SecondaryInvalidDepth * 0.5f;

        if (validSecondaryDepth && secondaryDepth < primaryLinearDepth - DepthEpsilon)
            outputDepth = secondaryDepth;
    }

    OutputLinearDepth[pixel] = outputDepth;
}
