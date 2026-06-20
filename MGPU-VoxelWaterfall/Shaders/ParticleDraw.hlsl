#include "Common.hlsl"

ConstantBuffer<EmitterData> EmitterBuffer : register(b0, space1);
StructuredBuffer<ParticleData> Particles : register(t0, space1);
StructuredBuffer<uint> RenderingParticles : register(t1, space1);

struct VertexOut
{
    float3 PositionW : POSITION;
};

VertexOut VS(uint vertexID : SV_VertexID)
{
    const uint particleIndex = RenderingParticles[vertexID];
    const ParticleData particle = Particles[particleIndex];

    VertexOut output;
    output.PositionW = mul(float4(particle.Position, 1.0f), objectBuffer.World).xyz;
    return output;
}

struct GeoOut
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
};

void EmitCubeVertex(float3 center, float3 offset, float3 normal, inout TriangleStream<GeoOut> stream)
{
    GeoOut output;
    output.PositionH = mul(float4(center + offset, 1.0f), worldBuffer.ViewProj);
    output.PositionW = center + offset;
    output.NormalW = normal;
    stream.Append(output);
}

[maxvertexcount(24)]
void GS(point VertexOut input[1], inout TriangleStream<GeoOut> stream)
{
    const float h = max(EmitterBuffer.VoxelSize, 0.05f) * 0.38f;
    const float3 center = input[0].PositionW;

    EmitCubeVertex(center, float3(-h, -h,  h), float3(0, 0, 1), stream);
    EmitCubeVertex(center, float3(-h,  h,  h), float3(0, 0, 1), stream);
    EmitCubeVertex(center, float3( h, -h,  h), float3(0, 0, 1), stream);
    EmitCubeVertex(center, float3( h,  h,  h), float3(0, 0, 1), stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3( h, -h, -h), float3(0, 0, -1), stream);
    EmitCubeVertex(center, float3( h,  h, -h), float3(0, 0, -1), stream);
    EmitCubeVertex(center, float3(-h, -h, -h), float3(0, 0, -1), stream);
    EmitCubeVertex(center, float3(-h,  h, -h), float3(0, 0, -1), stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h, -h, -h), float3(-1, 0, 0), stream);
    EmitCubeVertex(center, float3(-h,  h, -h), float3(-1, 0, 0), stream);
    EmitCubeVertex(center, float3(-h, -h,  h), float3(-1, 0, 0), stream);
    EmitCubeVertex(center, float3(-h,  h,  h), float3(-1, 0, 0), stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(h, -h,  h), float3(1, 0, 0), stream);
    EmitCubeVertex(center, float3(h,  h,  h), float3(1, 0, 0), stream);
    EmitCubeVertex(center, float3(h, -h, -h), float3(1, 0, 0), stream);
    EmitCubeVertex(center, float3(h,  h, -h), float3(1, 0, 0), stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h, h,  h), float3(0, 1, 0), stream);
    EmitCubeVertex(center, float3(-h, h, -h), float3(0, 1, 0), stream);
    EmitCubeVertex(center, float3( h, h,  h), float3(0, 1, 0), stream);
    EmitCubeVertex(center, float3( h, h, -h), float3(0, 1, 0), stream);
    stream.RestartStrip();

    EmitCubeVertex(center, float3(-h, -h, -h), float3(0, -1, 0), stream);
    EmitCubeVertex(center, float3(-h, -h,  h), float3(0, -1, 0), stream);
    EmitCubeVertex(center, float3( h, -h, -h), float3(0, -1, 0), stream);
    EmitCubeVertex(center, float3( h, -h,  h), float3(0, -1, 0), stream);
    stream.RestartStrip();
}

float4 PS(GeoOut input) : SV_Target
{
    const float3 normal = normalize(input.NormalW);
    const float3 lightDir = normalize(float3(0.35f, 0.85f, -0.45f));
    const float diffuse = saturate(dot(normal, lightDir));
    const float topFace = saturate(normal.y * 0.5f + 0.5f);
    const float heightFade = saturate((input.PositionW.y - EmitterBuffer.FloorHeight) /
                                      max(EmitterBuffer.SpawnHeight - EmitterBuffer.FloorHeight, 1.0f));
    const float3 deepWater = EmitterBuffer.Color.rgb;
    const float3 foamTint = float3(0.55f, 0.85f, 1.0f);
    const float3 baseColor = lerp(deepWater, foamTint, 0.12f + 0.18f * topFace + 0.10f * heightFade);
    const float lighting = 0.48f + 0.42f * diffuse + 0.10f * topFace;
    return float4(baseColor * lighting, 1.0f);
}
