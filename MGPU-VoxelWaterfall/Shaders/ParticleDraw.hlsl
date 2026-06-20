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
    float3 NormalW : NORMAL;
};

void EmitCubeVertex(float3 center, float3 offset, float3 normal, inout TriangleStream<GeoOut> stream)
{
    GeoOut output;
    output.PositionH = mul(float4(center + offset, 1.0f), worldBuffer.ViewProj);
    output.NormalW = normal;
    stream.Append(output);
}

[maxvertexcount(24)]
void GS(point VertexOut input[1], inout TriangleStream<GeoOut> stream)
{
    const float h = max(EmitterBuffer.VoxelSize, 0.05f) * 0.48f;
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
    const float light = 0.65f + 0.35f * saturate(dot(normalize(input.NormalW), normalize(float3(0.3f, 0.8f, -0.4f))));
    return float4(EmitterBuffer.Color.rgb * light, 1.0f);
}
