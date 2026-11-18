Texture2D g_tex : register(t0);
SamplerState g_sampler : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.uv = float2((p[vid].x + 1.0) * 0.5, 1.0 - (p[vid].y + 1.0) * 0.5);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(g_tex.SampleLevel(g_sampler, i.uv, 0.0).rgb, 1.0);
}
