#include "Common.hlsl"

ConstantBuffer<EmitterData> EmitterBuffer : register(b0, space1);
StructuredBuffer<ParticleData> Particles : register(t0, space1);
StructuredBuffer<VoxelLodRenderItem> RenderingParticles : register(t1, space1);

struct VertexOut
{
    float3 PositionW : POSITION;
    float3 HalfExtentW : TEXCOORD0;
    float LodLevel : TEXCOORD1;
    uint MaterialId : TEXCOORD2;
};

VertexOut VS(uint vertexID : SV_VertexID)
{
    const VoxelLodRenderItem renderItem = RenderingParticles[vertexID];
    float3 center = lerp(renderItem.PreviousCenter, renderItem.CurrentCenter,
                         saturate(EmitterBuffer.InterpolationAlpha));
    const float3 halfExtent = float3(renderItem.HalfExtentX, renderItem.HalfExtentY, renderItem.HalfExtentZ);

    VertexOut output = (VertexOut)0;
    output.PositionW = mul(float4(center, 1.0f), objectBuffer.World).xyz;
    output.HalfExtentW = halfExtent;
    output.LodLevel = (float)min(renderItem.LodLevel, 2u);
    output.MaterialId = renderItem.MaterialId;
    return output;
}

struct GeoOut
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float LodLevel : TEXCOORD0;
    uint MaterialId : TEXCOORD1;
};

void EmitCubeVertex(float3 center, float3 offset, float3 normal, float lodLevel, uint materialId,
                    inout TriangleStream<GeoOut> stream)
{
    GeoOut output = (GeoOut)0;
    output.PositionH = mul(float4(center + offset, 1.0f), worldBuffer.ViewProj);
    output.PositionW = center + offset;
    output.NormalW = normal;
    output.LodLevel = lodLevel;
    output.MaterialId = materialId;
    stream.Append(output);
}

[maxvertexcount(24)]
void GS(point VertexOut input[1], inout TriangleStream<GeoOut> stream)
{
    const float lodLevel = min(input[0].LodLevel, 2.0f);
    const uint materialId = input[0].MaterialId;
    const float3 center = input[0].PositionW;
    const float3 h = max(input[0].HalfExtentW, max(EmitterBuffer.VoxelSize, 0.05f) * 0.5f);

    EmitCubeVertex(center, float3(-h.x, -h.y,  h.z), float3(0, 0, 1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x,  h.y,  h.z), float3(0, 0, 1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x, -h.y,  h.z), float3(0, 0, 1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x,  h.y,  h.z), float3(0, 0, 1), lodLevel, materialId, stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3( h.x, -h.y, -h.z), float3(0, 0, -1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x,  h.y, -h.z), float3(0, 0, -1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x, -h.y, -h.z), float3(0, 0, -1), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x,  h.y, -h.z), float3(0, 0, -1), lodLevel, materialId, stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h.x, -h.y, -h.z), float3(-1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x,  h.y, -h.z), float3(-1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x, -h.y,  h.z), float3(-1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x,  h.y,  h.z), float3(-1, 0, 0), lodLevel, materialId, stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(h.x, -h.y,  h.z), float3(1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(h.x,  h.y,  h.z), float3(1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(h.x, -h.y, -h.z), float3(1, 0, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(h.x,  h.y, -h.z), float3(1, 0, 0), lodLevel, materialId, stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h.x, h.y,  h.z), float3(0, 1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x, h.y, -h.z), float3(0, 1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x, h.y,  h.z), float3(0, 1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x, h.y, -h.z), float3(0, 1, 0), lodLevel, materialId, stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h.x, -h.y, -h.z), float3(0, -1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3(-h.x, -h.y,  h.z), float3(0, -1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x, -h.y, -h.z), float3(0, -1, 0), lodLevel, materialId, stream);
    EmitCubeVertex(center, float3( h.x, -h.y,  h.z), float3(0, -1, 0), lodLevel, materialId, stream);
    stream.RestartStrip();
}

float3 ComputeVoxelColor(GeoOut input)
{
    const float3 normal = normalize(input.NormalW);
    const float3 lightDir = normalize(float3(0.35f, 0.85f, -0.45f));
    const float diffuse = saturate(dot(normal, lightDir));
    const float topFace = saturate(normal.y * 0.5f + 0.5f);
    const float heightFade = saturate((input.PositionW.y - EmitterBuffer.FloorHeight) /
                                      max(EmitterBuffer.SpawnHeight - EmitterBuffer.FloorHeight, 1.0f));
    float3 baseColor = EmitterBuffer.Color.rgb;
    if (EmitterBuffer.StreamKind == 1u)
    {
        if (input.MaterialId == 1u)
            baseColor = float3(0.42f, 0.47f, 0.40f);
        else if (input.MaterialId == 2u)
            baseColor = float3(0.34f, 0.36f, 0.34f);
        else if (input.MaterialId == 3u)
            baseColor = float3(0.20f, 0.34f, 0.38f);
        else if (input.MaterialId == 4u)
            baseColor = float3(0.50f, 0.48f, 0.40f);
    }
    else
    {
        const float3 foamTint = float3(0.55f, 0.85f, 1.0f);
        baseColor = lerp(baseColor, foamTint, 0.12f + 0.18f * topFace + 0.10f * heightFade);
    }
    const float lighting = 0.48f + 0.42f * diffuse + 0.10f * topFace;
    float3 outputColor = baseColor * lighting;

    if (EmitterBuffer.SpatialLodDebugMode == 1u)
    {
        if (input.LodLevel < 0.5f)
            outputColor = float3(0.15f, 0.80f, 1.0f);
        else if (input.LodLevel < 1.5f)
            outputColor = float3(0.25f, 1.0f, 0.35f);
        else
            outputColor = float3(1.0f, 0.75f, 0.15f);
    }
    else if (EmitterBuffer.SpatialLodDebugMode == 2u)
    {
        if (EmitterBuffer.StreamKind == 1u)
        {
            outputColor = EmitterBuffer.AdapterOwner == 1u
                              ? float3(1.0f, 0.58f, 0.12f)
                              : float3(0.20f, 0.72f, 0.35f);
        }
        else
        {
            outputColor = EmitterBuffer.AdapterOwner == 1u
                              ? float3(1.0f, 0.20f, 0.25f)
                              : float3(0.20f, 0.52f, 1.0f);
        }
    }

    return outputColor;
}

float4 PS(GeoOut input) : SV_Target
{
    return float4(ComputeVoxelColor(input), 1.0f);
}

struct SecondaryPixelOut
{
    float4 Color : SV_Target0;
    float LinearDepth : SV_Target1;
};

SecondaryPixelOut PSSecondary(GeoOut input)
{
    SecondaryPixelOut output;
    output.Color = float4(ComputeVoxelColor(input), 1.0f);

    const float3 viewPosition = mul(float4(input.PositionW, 1.0f), worldBuffer.View).xyz;
    output.LinearDepth = max(viewPosition.z, 0.0f);
    return output;
}
