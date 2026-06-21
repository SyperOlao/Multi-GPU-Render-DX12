Texture2D gPrimaryColor : register(t0);
Texture2D gPrimaryDepth : register(t1);
Texture2D gSecondaryColor : register(t2);
Texture2D gSecondaryLinearDepth : register(t3);

SamplerState gsamPointClamp : register(s1);

cbuffer CompositeConstants : register(b0)
{
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gDepthEpsilon;
    uint gDebugView;
    float gSecondaryInvalidDepth;
    float gPadding;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VertexOut VS(uint vertexId : SV_VertexID)
{
    VertexOut output;
    output.TexC = float2((vertexId << 1) & 2, vertexId & 2);
    output.PosH = float4(output.TexC * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float HardwareDepthToLinearViewDepth(float hardwareDepth)
{
    const float denominator = max(gFarZ - hardwareDepth * (gFarZ - gNearZ), 1.0e-6f);
    return (gNearZ * gFarZ) / denominator;
}

float3 Heat(float value)
{
    value = saturate(value);
    return float3(value, 1.0f - abs(value - 0.5f) * 2.0f, 1.0f - value);
}

float4 PS(VertexOut pin) : SV_Target
{
    const float4 primaryColor = gPrimaryColor.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);
    const float primaryHardwareDepth = gPrimaryDepth.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).r;
    const float primaryLinearDepth = HardwareDepthToLinearViewDepth(primaryHardwareDepth);

    const float4 secondaryColor = gSecondaryColor.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);
    const float secondaryLinearDepth = gSecondaryLinearDepth.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).r;
    const bool hasSecondary = secondaryColor.a > 0.0f;
    const bool validSecondaryDepth =
        hasSecondary &&
        secondaryLinearDepth > 0.0f &&
        secondaryLinearDepth < gSecondaryInvalidDepth * 0.5f;
    const bool secondaryWins =
        validSecondaryDepth &&
        secondaryLinearDepth < primaryLinearDepth - gDepthEpsilon;

    if (gDebugView == 1)
        return primaryColor;
    if (gDebugView == 2)
        return hasSecondary ? float4(secondaryColor.rgb, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (gDebugView == 3)
        return validSecondaryDepth ? float4(Heat(secondaryLinearDepth / gFarZ), 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (gDebugView == 4)
        return float4(Heat(primaryLinearDepth / gFarZ), 1.0f);
    if (gDebugView == 5)
    {
        if (!hasSecondary)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        return secondaryWins ? float4(0.0f, 0.85f, 0.2f, 1.0f) : float4(0.1f, 0.35f, 1.0f, 1.0f);
    }
    if (gDebugView == 6)
    {
        if (!validSecondaryDepth)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        const float diff = (primaryLinearDepth - secondaryLinearDepth) / max(gFarZ - gNearZ, 1.0f);
        return diff >= 0.0f
                   ? float4(saturate(diff * 64.0f), 0.0f, 0.0f, 1.0f)
                   : float4(0.0f, 0.0f, saturate(-diff * 64.0f), 1.0f);
    }

    if (!hasSecondary)
        return primaryColor;

    return secondaryWins ? float4(secondaryColor.rgb, 1.0f) : primaryColor;
}

