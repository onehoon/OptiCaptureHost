#pragma once

static const char kBlitHlsl[] = R"HLSL(
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = corner;
    return output;
}
float4 PSMain(VertexOutput input) : SV_TARGET
{
    return Source.Sample(LinearClamp, input.uv);
}
)HLSL";
