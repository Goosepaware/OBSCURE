// OBScure live decoder shader.
// Output pixels are in guard-free restored space. A decode map converts
// (tile, output intra) -> (captured dest tile, dest intra); a homography then
// samples the captured transport texture.

cbuffer DecodeParams : register(b0)
{
    uint g_canvasW;
    uint g_canvasH;
    uint g_contentX;
    uint g_contentY;
    uint g_cell;
    uint g_guard;
    uint g_cols;
    uint g_rows;
    uint g_inner;
    uint g_refW;
    uint g_refH;
    uint g_captureW;
    uint g_captureH;
    float g_margin;
    uint g_useColor;
    uint g_useDih;
    float4 gH0;
    float4 gH1;
    float4 gH2;
};

StructuredBuffer<uint> g_decLut : register(t0);       // source cell -> dest | color<<16 | dih<<19
StructuredBuffer<float2> g_decodeMap : register(t1);  // dih, output intra -> dest intra (+0.5)
Texture2D g_capture : register(t2);
Texture2D g_restored : register(t3);
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

float3 applyColor(float3 c, uint code)
{
    float3 o = c;
    uint perm = code >> 1;
    if (perm == 0) o = c;
    else if (perm == 1) o = c.bgr;   // swap R/B
    else if (perm == 2) o = c.grb;   // swap R/G
    else o = c.rbg;                  // swap G/B
    if ((code & 1u) != 0u) o = 1.0 - o;
    return o;
}

float4 PSDecode(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    p = clamp(p, int2(0, 0), int2((int)g_refW - 1, (int)g_refH - 1));
    int inner = (int)g_inner;
    int2 bcell = int2(p.x / inner, p.y / inner);
    int2 intra = int2(p.x % inner, p.y % inner);

    uint b = (uint)(bcell.y * (int)g_cols + bcell.x);
    uint lut = g_decLut[b];
    uint d = lut & 0xFFFFu;
    uint colorCode = (lut >> 16) & 7u;
    uint dih = (lut >> 19) & 7u;

    float2 dm = g_decodeMap[dih * (uint)(inner * inner) +
                             (uint)(intra.y * inner + intra.x)];
    float2 canon = float2(
        (float)((int)g_contentX + (int)(d % g_cols) * (int)g_cell + (int)g_guard) + dm.x,
        (float)((int)g_contentY + (int)(d / g_cols) * (int)g_cell + (int)g_guard) + dm.y);

    float3 hp = float3(canon, 1.0);
    float3 q = float3(dot(gH0.xyz, hp), dot(gH1.xyz, hp), dot(gH2.xyz, hp));
    float2 uv = q.xy / max(q.z, 1e-8) / float2((float)g_captureW, (float)g_captureH);

    float3 col = g_capture.SampleLevel(g_sampler, uv, 0.0).rgb;
    if (g_useColor != 0u) {
        col = applyColor(col, colorCode);
    }
    return float4(col, 1.0);
}

float4 PSBlit(VSOut i) : SV_Target
{
    return g_restored.SampleLevel(g_sampler, i.uv, 0.0);
}
