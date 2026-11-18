#include "live/decode_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/transform.h"
#include "live/d3d_helpers.h"

namespace obs {

namespace {

struct alignas(16) DecodeParams {
    uint32_t canvasW;
    uint32_t canvasH;
    uint32_t contentX;
    uint32_t contentY;
    uint32_t cell;
    uint32_t guard;
    uint32_t cols;
    uint32_t rows;
    uint32_t inner;
    uint32_t refW;
    uint32_t refH;
    uint32_t captureW;
    uint32_t captureH;
    float margin;
    uint32_t useColor;
    uint32_t useDih;
    float H0[4];
    float H1[4];
    float H2[4];
};
static_assert(sizeof(DecodeParams) == 112, "decode params size");

std::vector<uint32_t> build_src_from_dst(const Layout& layout) {
    const int inner = layout.inner();
    std::vector<uint32_t> table(static_cast<size_t>(8 * inner * inner));
    cv::Mat probe(inner, inner, CV_16U);
    for (int code = 0; code < 8; ++code) {
        for (int y = 0; y < inner; ++y) {
            for (int x = 0; x < inner; ++x) {
                probe.at<ushort>(y, x) = static_cast<ushort>(y * inner + x);
            }
        }
        cv::Mat transformed;
        apply_dihedral(probe, transformed, code);
        for (int y = 0; y < inner; ++y) {
            for (int x = 0; x < inner; ++x) {
                table[static_cast<size_t>(code * inner * inner + y * inner + x)] =
                    transformed.at<ushort>(y, x);
            }
        }
    }
    return table;
}

std::vector<uint32_t> build_dec_lut(const Layout& layout, const KeyMaterial& key,
                                    bool use_color, bool use_dihedral) {
    std::vector<uint32_t> lut(static_cast<size_t>(layout.blocks()), 0);
    for (int d = 0; d < layout.blocks(); ++d) {
        const int b = key.perm[static_cast<size_t>(d)];
        const uint32_t color =
            use_color ? (key.color[static_cast<size_t>(d)] & 7u) : 0u;
        const uint32_t dih =
            use_dihedral ? (key.dihedral[static_cast<size_t>(d)] & 7u) : 0u;
        lut[static_cast<size_t>(b)] =
            static_cast<uint32_t>(d) | (color << 16) | (dih << 19);
    }
    return lut;
}

std::vector<std::pair<float, float>> build_decode_map(const Layout& layout, int margin) {
    const int inner = layout.inner();
    const auto src_from_dst = build_src_from_dst(layout);
    std::vector<uint32_t> dst_from_src(static_cast<size_t>(8 * inner * inner), 0);
    for (int code = 0; code < 8; ++code) {
        for (int dest = 0; dest < inner * inner; ++dest) {
            const uint32_t src =
                src_from_dst[static_cast<size_t>(code * inner * inner + dest)];
            dst_from_src[static_cast<size_t>(code * inner * inner + src)] =
                static_cast<uint32_t>(dest);
        }
    }

    std::vector<std::pair<float, float>> map(static_cast<size_t>(8 * inner * inner));
    const float scale = static_cast<float>(inner - 2 * margin) / static_cast<float>(inner);
    for (int code = 0; code < 8; ++code) {
        for (int v = 0; v < inner; ++v) {
            for (int u = 0; u < inner; ++u) {
                const float su = static_cast<float>(margin) + (u + 0.5f) * scale - 0.5f;
                const float sv = static_cast<float>(margin) + (v + 0.5f) * scale - 0.5f;
                const int sx = std::clamp(static_cast<int>(std::lround(su)), 0, inner - 1);
                const int sy = std::clamp(static_cast<int>(std::lround(sv)), 0, inner - 1);
                const uint32_t dest =
                    dst_from_src[static_cast<size_t>(code * inner * inner + sy * inner + sx)];
                map[static_cast<size_t>(code * inner * inner + v * inner + u)] = {
                    static_cast<float>(dest % inner) + 0.5f,
                    static_cast<float>(dest / inner) + 0.5f,
                };
            }
        }
    }
    return map;
}

}  // namespace

bool DecodePipeline::init(ID3D11Device* device, ID3D11DeviceContext* context,
                          const Layout& layout, const KeyMaterial& key,
                          bool use_color, bool use_dihedral,
                          const std::wstring& shader_path, std::string* error) {
    device_ = device;
    context_ = context;
    layout_ = layout;
    margin_ = -1;
    use_color_ = use_color;
    use_dihedral_ = use_dihedral;

    auto vs_blob = compile_shader(shader_path, "VSMain", "vs_5_0", error);
    auto ps_blob = compile_shader(shader_path, "PSDecode", "ps_5_0", error);
    if (!vs_blob || !ps_blob) return false;
    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         ps_.GetAddressOf()))) {
        if (error) *error = "failed to create decode shaders";
        return false;
    }

    const auto lut = build_dec_lut(layout_, key, use_color_, use_dihedral_);
    auto lut_buf = create_structured_buffer(device, lut.size() * sizeof(uint32_t),
                                            sizeof(uint32_t), lut.data());
    if (!lut_buf) {
        if (error) *error = "failed to create decode LUT";
        return false;
    }
    lut_srv_ = create_buffer_srv(device, lut_buf.Get(), static_cast<UINT>(lut.size()));
    if (!lut_srv_) {
        if (error) *error = "failed to create decode LUT SRV";
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(DecodeParams);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, cb_.GetAddressOf()))) {
        if (error) *error = "failed to create decode constant buffer";
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler_desc, sampler_.GetAddressOf()))) {
        if (error) *error = "failed to create decode sampler";
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc, rasterizer_.GetAddressOf()))) {
        if (error) *error = "failed to create decode rasterizer";
        return false;
    }

    restored_tex_ = create_texture(device, layout_.ref_w(), layout_.ref_h(),
                                   DXGI_FORMAT_B8G8R8A8_UNORM,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                   nullptr, 0);
    if (!restored_tex_ ||
        FAILED(device->CreateRenderTargetView(restored_tex_.Get(), nullptr,
                                              restored_rtv_.GetAddressOf()))) {
        if (error) *error = "failed to create restored texture";
        return false;
    }
    restored_srv_ = create_srv(device, restored_tex_.Get());
    if (!restored_srv_) {
        if (error) *error = "failed to create restored SRV";
        return false;
    }

    set_margin(0);
    return map_srv_ != nullptr;
}

void DecodePipeline::set_margin(int margin) {
    if (margin == margin_ && map_srv_) return;
    margin_ = margin;
    const auto map = build_decode_map(layout_, margin);
    map_buf_ = create_structured_buffer(device_.Get(), map.size() * sizeof(MapEntry),
                                        sizeof(MapEntry), map.data());
    map_srv_ = map_buf_ ? create_buffer_srv(device_.Get(), map_buf_.Get(),
                                            static_cast<UINT>(map.size()))
                        : nullptr;
}

void DecodePipeline::render(ID3D11ShaderResourceView* capture, int capture_w,
                            int capture_h, const cv::Mat& H) {
    if (!context_ || !capture || H.empty()) return;
    DecodeParams params{};
    params.canvasW = static_cast<uint32_t>(layout_.canvas_w);
    params.canvasH = static_cast<uint32_t>(layout_.canvas_h);
    params.contentX = static_cast<uint32_t>(layout_.content_x);
    params.contentY = static_cast<uint32_t>(layout_.content_y);
    params.cell = static_cast<uint32_t>(layout_.cell);
    params.guard = static_cast<uint32_t>(layout_.guard);
    params.cols = static_cast<uint32_t>(layout_.cols());
    params.rows = static_cast<uint32_t>(layout_.rows());
    params.inner = static_cast<uint32_t>(layout_.inner());
    params.refW = static_cast<uint32_t>(layout_.ref_w());
    params.refH = static_cast<uint32_t>(layout_.ref_h());
    params.captureW = static_cast<uint32_t>(capture_w);
    params.captureH = static_cast<uint32_t>(capture_h);
    params.margin = static_cast<float>(margin_);
    params.useColor = use_color_ ? 1u : 0u;
    params.useDih = use_dihedral_ ? 1u : 0u;
    params.H0[0] = static_cast<float>(H.at<double>(0, 0));
    params.H0[1] = static_cast<float>(H.at<double>(0, 1));
    params.H0[2] = static_cast<float>(H.at<double>(0, 2));
    params.H0[3] = 0.0f;
    params.H1[0] = static_cast<float>(H.at<double>(1, 0));
    params.H1[1] = static_cast<float>(H.at<double>(1, 1));
    params.H1[2] = static_cast<float>(H.at<double>(1, 2));
    params.H1[3] = 0.0f;
    params.H2[0] = static_cast<float>(H.at<double>(2, 0));
    params.H2[1] = static_cast<float>(H.at<double>(2, 1));
    params.H2[2] = static_cast<float>(H.at<double>(2, 2));
    params.H2[3] = 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(cb_.Get(), 0);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(layout_.ref_w());
    vp.Height = static_cast<float>(layout_.ref_h());
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[3] = {lut_srv_.Get(), map_srv_.Get(), capture};
    ID3D11Buffer* cbs[1] = {cb_.Get()};
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};

    context_->OMSetRenderTargets(1, restored_rtv_.GetAddressOf(), nullptr);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterizer_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, cbs);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[3] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, nulls);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace obs
