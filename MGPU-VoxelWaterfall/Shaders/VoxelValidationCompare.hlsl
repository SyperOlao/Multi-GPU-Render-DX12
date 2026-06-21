Texture2D<float4> SingleColor : register(t0);
Texture2D<float> SingleLinearDepth : register(t1);
Texture2D<float4> MultiColor : register(t2);
Texture2D<float> MultiLinearDepth : register(t3);

struct ValidationTileStats
{
    uint PixelCount;
    uint ColorMismatchCount;
    uint DepthMismatchCount;
    float ColorAbsoluteErrorSum;
    float ColorSquaredErrorSum;
    float MaxColorError;
    float DepthSquaredErrorSum;
    float Padding;
};

RWStructuredBuffer<ValidationTileStats> TileStats : register(u0);

cbuffer ValidationConstants : register(b0)
{
    uint2 RenderSize;
    float ColorTolerance;
    float DepthTolerance;
};

groupshared uint SharedPixelCount[64];
groupshared uint SharedColorMismatchCount[64];
groupshared uint SharedDepthMismatchCount[64];
groupshared float SharedColorAbsoluteErrorSum[64];
groupshared float SharedColorSquaredErrorSum[64];
groupshared float SharedMaxColorError[64];
groupshared float SharedDepthSquaredErrorSum[64];

[numthreads(8, 8, 1)]
void CS(uint3 groupId : SV_GroupID,
        uint3 groupThreadId : SV_GroupThreadID,
        uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const bool inBounds =
        dispatchThreadId.x < RenderSize.x && dispatchThreadId.y < RenderSize.y;

    float colorAbsoluteError = 0.0f;
    float colorSquaredError = 0.0f;
    float maxColorError = 0.0f;
    float depthSquaredError = 0.0f;
    uint colorMismatch = 0u;
    uint depthMismatch = 0u;

    if (inBounds)
    {
        const uint2 pixel = dispatchThreadId.xy;
        const float4 singleColor = SingleColor.Load(uint3(pixel, 0));
        const float4 multiColor = MultiColor.Load(uint3(pixel, 0));
        const float singleDepth = SingleLinearDepth.Load(uint3(pixel, 0));
        const float multiDepth = MultiLinearDepth.Load(uint3(pixel, 0));

        const float4 colorError = abs(singleColor - multiColor);
        maxColorError = max(max(colorError.r, colorError.g), max(colorError.b, colorError.a));
        colorAbsoluteError = dot(colorError, float4(0.25f, 0.25f, 0.25f, 0.25f));
        colorSquaredError = dot(colorError * colorError, float4(0.25f, 0.25f, 0.25f, 0.25f));

        const float depthError = abs(singleDepth - multiDepth);
        depthSquaredError = depthError * depthError;

        colorMismatch = maxColorError > ColorTolerance ? 1u : 0u;
        depthMismatch = depthError > DepthTolerance ? 1u : 0u;
    }

    SharedPixelCount[lane] = inBounds ? 1u : 0u;
    SharedColorMismatchCount[lane] = colorMismatch;
    SharedDepthMismatchCount[lane] = depthMismatch;
    SharedColorAbsoluteErrorSum[lane] = colorAbsoluteError;
    SharedColorSquaredErrorSum[lane] = colorSquaredError;
    SharedMaxColorError[lane] = maxColorError;
    SharedDepthSquaredErrorSum[lane] = depthSquaredError;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            SharedPixelCount[lane] += SharedPixelCount[lane + stride];
            SharedColorMismatchCount[lane] += SharedColorMismatchCount[lane + stride];
            SharedDepthMismatchCount[lane] += SharedDepthMismatchCount[lane + stride];
            SharedColorAbsoluteErrorSum[lane] += SharedColorAbsoluteErrorSum[lane + stride];
            SharedColorSquaredErrorSum[lane] += SharedColorSquaredErrorSum[lane + stride];
            SharedMaxColorError[lane] = max(SharedMaxColorError[lane], SharedMaxColorError[lane + stride]);
            SharedDepthSquaredErrorSum[lane] += SharedDepthSquaredErrorSum[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        const uint groupsX = (RenderSize.x + 7u) / 8u;
        const uint tileIndex = groupId.y * groupsX + groupId.x;
        ValidationTileStats stats;
        stats.PixelCount = SharedPixelCount[0];
        stats.ColorMismatchCount = SharedColorMismatchCount[0];
        stats.DepthMismatchCount = SharedDepthMismatchCount[0];
        stats.ColorAbsoluteErrorSum = SharedColorAbsoluteErrorSum[0];
        stats.ColorSquaredErrorSum = SharedColorSquaredErrorSum[0];
        stats.MaxColorError = SharedMaxColorError[0];
        stats.DepthSquaredErrorSum = SharedDepthSquaredErrorSum[0];
        stats.Padding = 0.0f;
        TileStats[tileIndex] = stats;
    }
}
