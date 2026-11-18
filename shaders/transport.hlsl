// OBScure live transport shader.
// One full-screen pass: border texels come from the precomputed border texture,
// content texels are gathered from the captured source through the keyed tile map.

cbuffer Params : register(b0)
{
    uint g_canvasW;
    uint g_canvasH;
    uint g_contentX;
    uint g_contentY;
    uint g_contentW;
    uint g_contentH;
    uint g_cell;
    uint g_guard;
    uint g_cols;
    uint g_rows;
    uint g_inner;
    uint g_refW;
    uint g_refH;
    float g_extraScale;
    uint g_pad0;
    uint g_pad1;
};

StructuredBuffer<uint> g_lut : register(t0);          // dest cell -> src | color<<16 | dih<<19
StructuredBuffer<uint> g_srcFromDst : register(t1);   // dih, dest intra -> src intra
Texture2D g_src : register(t2);
Texture2D g_border : register(t3);
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

float4 PSMain(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    p = clamp(p, int2(0, 0), int2((int)g_canvasW - 1, (int)g_canvasH - 1));

    if (p.x < (int)g_contentX || p.y < (int)g_contentY ||
        p.x >= (int)(g_contentX + g_contentW) ||
        p.y >= (int)(g_contentY + g_contentH))
    {
        return g_border.SampleLevel(g_sampler, i.uv, 0.0);
    }

    int2 c = p - int2((int)g_contentX, (int)g_contentY);
    int cell = (int)g_cell;
    int guard = (int)g_guard;
    int inner = (int)g_inner;
    int2 cellIdx = int2(c.x / cell, c.y / cell);
    int2 intra = int2(c.x % cell, c.y % cell);

    if (intra.x < guard || intra.y < guard ||
        intra.x >= guard + inner || intra.y >= guard + inner)
    {
        return float4(12.0 / 255.0, 12.0 / 255.0, 12.0 / 255.0, 1.0);
    }

    uint d = (uint)(cellIdx.y * (int)g_cols + cellIdx.x);
    uint lut = g_lut[d];
    uint b = lut & 0xFFFFu;
    uint colorCode = (lut >> 16) & 7u;
    uint dih = (lut >> 19) & 7u;

    int ix = intra.x - guard;
    int iy = intra.y - guard;
    uint mapIdx = dih * (uint)(inner * inner) + (uint)(iy * inner + ix);
    uint s = g_srcFromDst[mapIdx];
    // The table stores a linear intra-tile index (y * inner + x).
    int sx = (int)(s % (uint)inner);
    int sy = (int)(s / (uint)inner);

    int bcellX = (int)(b % g_cols);
    int bcellY = (int)(b / g_cols);
    int refX = bcellX * inner + sx;
    int refY = bcellY * inner + sy;

    float2 refSize = float2((float)g_refW, (float)g_refH);
    float2 uv = (float2((float)refX, (float)refY) + 0.5) / refSize;

    // Source footprint: roughly area-filter the full capture down to the
    // guard-free reference grid. extraScale widens the footprint a little.
    uint sw = 0;
    uint sh = 0;
    g_src.GetDimensions(sw, sh);
    float2 texel = 1.0 / float2((float)sw, (float)sh);
    float footprint = max(1.0, g_extraScale) * 0.5;
    float2 o = texel * footprint;

    float3 col = g_src.SampleLevel(g_sampler, uv + float2(-o.x, -o.y), 0.0).rgb;
    col += g_src.SampleLevel(g_sampler, uv + float2( o.x, -o.y), 0.0).rgb;
    col += g_src.SampleLevel(g_sampler, uv + float2(-o.x,  o.y), 0.0).rgb;
    col += g_src.SampleLevel(g_sampler, uv + float2( o.x,  o.y), 0.0).rgb;
    col *= 0.25;

    col = applyColor(col, colorCode);
    return float4(col, 1.0);
}
