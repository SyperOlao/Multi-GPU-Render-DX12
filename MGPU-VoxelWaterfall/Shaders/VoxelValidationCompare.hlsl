Texture2D<float4> SingleColor : register(t0);
Texture2D<float> SingleLinearDepth : register(t1);
Texture2D<float4> MultiColor : register(t2);
Texture2D<float> MultiLinearDepth : register(t3);

struct ValidationTileStats
{
    uint PixelCount;
    uint ForegroundUnionCount;
    uint ForegroundIntersectionCount;
    uint ColorMismatchCount;
    uint DepthMismatchCount;
    uint CombinedMismatchCount;
    uint CoverageMismatchCount;
    uint Padding0;
    float RgbAbsoluteErrorSum;
    float RgbSquaredErrorSum;
    float RgbMaxAbsoluteError;
    float AlphaAbsoluteErrorSum;
    float DepthAbsoluteErrorSum;
    float DepthSquaredErrorSum;
    float DepthRelativeErrorSum;
    float DepthMaxAbsoluteError;
};

RWStructuredBuffer<ValidationTileStats> TileStats : register(u0);
RWTexture2D<float4> DiffOutput : register(u1);

cbuffer ValidationConstants : register(b0)
{
    uint2 RenderSize;
    float ColorTolerance;
    float DepthTolerance;
    float ValidDepthMax;
    float Padding1;
};

groupshared uint SharedPixelCount[64];
groupshared uint SharedForegroundUnionCount[64];
groupshared uint SharedForegroundIntersectionCount[64];
groupshared uint SharedColorMismatchCount[64];
groupshared uint SharedDepthMismatchCount[64];
groupshared uint SharedCombinedMismatchCount[64];
groupshared uint SharedCoverageMismatchCount[64];
groupshared float SharedRgbAbsoluteErrorSum[64];
groupshared float SharedRgbSquaredErrorSum[64];
groupshared float SharedRgbMaxAbsoluteError[64];
groupshared float SharedAlphaAbsoluteErrorSum[64];
groupshared float SharedDepthAbsoluteErrorSum[64];
groupshared float SharedDepthSquaredErrorSum[64];
groupshared float SharedDepthRelativeErrorSum[64];
groupshared float SharedDepthMaxAbsoluteError[64];

bool IsValidDepth(float depth)
{
    return isfinite(depth) && depth > 0.0f && depth < ValidDepthMax;
}

[numthreads(8, 8, 1)]
void CS(uint3 groupId : SV_GroupID,
        uint3 groupThreadId : SV_GroupThreadID,
        uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const bool inBounds =
        dispatchThreadId.x < RenderSize.x && dispatchThreadId.y < RenderSize.y;

    uint foregroundUnion = 0u;
    uint foregroundIntersection = 0u;
    uint colorMismatch = 0u;
    uint depthMismatch = 0u;
    uint combinedMismatch = 0u;
    uint coverageMismatch = 0u;
    float rgbAbsoluteError = 0.0f;
    float rgbSquaredError = 0.0f;
    float rgbMaxAbsoluteError = 0.0f;
    float alphaAbsoluteError = 0.0f;
    float depthAbsoluteError = 0.0f;
    float depthSquaredError = 0.0f;
    float depthRelativeError = 0.0f;
    float depthMaxAbsoluteError = 0.0f;

    if (inBounds)
    {
        const uint2 pixel = dispatchThreadId.xy;
        const float4 singleColor = SingleColor.Load(uint3(pixel, 0));
        const float4 multiColor = MultiColor.Load(uint3(pixel, 0));
        const float singleDepth = SingleLinearDepth.Load(uint3(pixel, 0));
        const float multiDepth = MultiLinearDepth.Load(uint3(pixel, 0));
        const bool singleValidDepth = IsValidDepth(singleDepth);
        const bool multiValidDepth = IsValidDepth(multiDepth);

        foregroundUnion = (singleValidDepth || multiValidDepth) ? 1u : 0u;
        foregroundIntersection = (singleValidDepth && multiValidDepth) ? 1u : 0u;
        coverageMismatch = (singleValidDepth != multiValidDepth) ? 1u : 0u;

        const float3 rgbError = abs(singleColor.rgb - multiColor.rgb);
        rgbMaxAbsoluteError = max(rgbError.r, max(rgbError.g, rgbError.b));
        rgbAbsoluteError = (rgbError.r + rgbError.g + rgbError.b) / 3.0f;
        rgbSquaredError = dot(rgbError, rgbError) / 3.0f;
        alphaAbsoluteError = abs(singleColor.a - multiColor.a);
        colorMismatch = rgbMaxAbsoluteError > ColorTolerance ? 1u : 0u;

        if (singleValidDepth && multiValidDepth)
        {
            depthAbsoluteError = abs(singleDepth - multiDepth);
            depthSquaredError = depthAbsoluteError * depthAbsoluteError;
            depthRelativeError = depthAbsoluteError / max(abs(singleDepth), 1.0e-4f);
            depthMaxAbsoluteError = depthAbsoluteError;
            depthMismatch = depthAbsoluteError > DepthTolerance ? 1u : 0u;
        }

        combinedMismatch = (colorMismatch != 0u || depthMismatch != 0u || coverageMismatch != 0u) ? 1u : 0u;

        const float normalizedDepthError = saturate(depthAbsoluteError / max(DepthTolerance, 1.0e-6f));
        const float normalizedColorError = saturate(rgbMaxAbsoluteError / max(ColorTolerance, 1.0e-6f));
        DiffOutput[pixel] = coverageMismatch != 0u
                                ? float4(1.0f, 0.0f, 1.0f, 1.0f)
                                : float4(normalizedColorError, normalizedDepthError, 0.0f, 1.0f);
    }

    SharedPixelCount[lane] = inBounds ? 1u : 0u;
    SharedForegroundUnionCount[lane] = foregroundUnion;
    SharedForegroundIntersectionCount[lane] = foregroundIntersection;
    SharedColorMismatchCount[lane] = colorMismatch;
    SharedDepthMismatchCount[lane] = depthMismatch;
    SharedCombinedMismatchCount[lane] = combinedMismatch;
    SharedCoverageMismatchCount[lane] = coverageMismatch;
    SharedRgbAbsoluteErrorSum[lane] = rgbAbsoluteError;
    SharedRgbSquaredErrorSum[lane] = rgbSquaredError;
    SharedRgbMaxAbsoluteError[lane] = rgbMaxAbsoluteError;
    SharedAlphaAbsoluteErrorSum[lane] = alphaAbsoluteError;
    SharedDepthAbsoluteErrorSum[lane] = depthAbsoluteError;
    SharedDepthSquaredErrorSum[lane] = depthSquaredError;
    SharedDepthRelativeErrorSum[lane] = depthRelativeError;
    SharedDepthMaxAbsoluteError[lane] = depthMaxAbsoluteError;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            SharedPixelCount[lane] += SharedPixelCount[lane + stride];
            SharedForegroundUnionCount[lane] += SharedForegroundUnionCount[lane + stride];
            SharedForegroundIntersectionCount[lane] += SharedForegroundIntersectionCount[lane + stride];
            SharedColorMismatchCount[lane] += SharedColorMismatchCount[lane + stride];
            SharedDepthMismatchCount[lane] += SharedDepthMismatchCount[lane + stride];
            SharedCombinedMismatchCount[lane] += SharedCombinedMismatchCount[lane + stride];
            SharedCoverageMismatchCount[lane] += SharedCoverageMismatchCount[lane + stride];
            SharedRgbAbsoluteErrorSum[lane] += SharedRgbAbsoluteErrorSum[lane + stride];
            SharedRgbSquaredErrorSum[lane] += SharedRgbSquaredErrorSum[lane + stride];
            SharedRgbMaxAbsoluteError[lane] = max(SharedRgbMaxAbsoluteError[lane],
                                                  SharedRgbMaxAbsoluteError[lane + stride]);
            SharedAlphaAbsoluteErrorSum[lane] += SharedAlphaAbsoluteErrorSum[lane + stride];
            SharedDepthAbsoluteErrorSum[lane] += SharedDepthAbsoluteErrorSum[lane + stride];
            SharedDepthSquaredErrorSum[lane] += SharedDepthSquaredErrorSum[lane + stride];
            SharedDepthRelativeErrorSum[lane] += SharedDepthRelativeErrorSum[lane + stride];
            SharedDepthMaxAbsoluteError[lane] = max(SharedDepthMaxAbsoluteError[lane],
                                                    SharedDepthMaxAbsoluteError[lane + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        const uint groupsX = (RenderSize.x + 7u) / 8u;
        const uint tileIndex = groupId.y * groupsX + groupId.x;
        ValidationTileStats stats;
        stats.PixelCount = SharedPixelCount[0];
        stats.ForegroundUnionCount = SharedForegroundUnionCount[0];
        stats.ForegroundIntersectionCount = SharedForegroundIntersectionCount[0];
        stats.ColorMismatchCount = SharedColorMismatchCount[0];
        stats.DepthMismatchCount = SharedDepthMismatchCount[0];
        stats.CombinedMismatchCount = SharedCombinedMismatchCount[0];
        stats.CoverageMismatchCount = SharedCoverageMismatchCount[0];
        stats.Padding0 = 0u;
        stats.RgbAbsoluteErrorSum = SharedRgbAbsoluteErrorSum[0];
        stats.RgbSquaredErrorSum = SharedRgbSquaredErrorSum[0];
        stats.RgbMaxAbsoluteError = SharedRgbMaxAbsoluteError[0];
        stats.AlphaAbsoluteErrorSum = SharedAlphaAbsoluteErrorSum[0];
        stats.DepthAbsoluteErrorSum = SharedDepthAbsoluteErrorSum[0];
        stats.DepthSquaredErrorSum = SharedDepthSquaredErrorSum[0];
        stats.DepthRelativeErrorSum = SharedDepthRelativeErrorSum[0];
        stats.DepthMaxAbsoluteError = SharedDepthMaxAbsoluteError[0];
        TileStats[tileIndex] = stats;
    }
}
