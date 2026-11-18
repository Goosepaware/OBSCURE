#include "live/encode_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opencv2/imgproc.hpp>

#include "core/border.h"
#include "core/transform.h"
#include "live/d3d_helpers.h"

namespace obs {

namespace {

struct alignas(16) ShaderParams {
    uint32_t canvasW;
    uint32_t canvasH;
    uint32_t contentX;
    uint32_t contentY;
    uint32_t contentW;
    uint32_t contentH;
    uint32_t cell;
    uint32_t guard;
    uint32_t cols;
    uint32_t rows;
    uint32_t inner;
    uint32_t refW;
    uint32_t refH;
    float extraScale;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(ShaderParams) == 64, "shader params must be 64 bytes");

std::vector<uint32_t> build_lut(const Layout& layout, const KeyMaterial& key,
                                const TransportOptions& options) {
    std::vector<uint32_t> lut(static_cast<size_t>(layout.blocks()));
    for (int d = 0; d < layout.blocks(); ++d) {
        const uint32_t b = static_cast<uint32_t>(key.perm[static_cast<size_t>(d)]);
        const uint32_t color =
            options.use_color ? (key.color[static_cast<size_t>(d)] & 7u) : 0u;
        const uint32_t dih =
            options.use_dihedral ? (key.dihedral[static_cast<size_t>(d)] & 7u) : 0u;
        lut[static_cast<size_t>(d)] = b | (color << 16) | (dih << 19);
    }
    return lut;
}

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

}  // namespace

bool EncodePipeline::init(ID3D11Device* device, ID3D11DeviceContext* context,
                          const Layout& layout, const KeyMaterial& key, double extra_scale,
                          const TransportOptions& options,
                          const std::wstring& shader_path, std::string* error) {
    device_ = device;
    context_ = context;
    layout_ = layout;
    extra_scale_ = extra_scale;
    options_ = options;

    auto vs_blob = compile_shader(shader_path, "VSMain", "vs_5_0", error);
    auto ps_blob = compile_shader(shader_path, "PSMain", "ps_5_0", error);
    if (!vs_blob || !ps_blob) return false;

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         ps_.GetAddressOf()))) {
        if (error) *error = "failed to create encode shaders";
        return false;
    }

    const cv::Mat border = build_border(layout_, key, options_);
    cv::Mat border_bgra;
    cv::cvtColor(border, border_bgra, cv::COLOR_BGR2BGRA);
    auto border_tex = create_texture(
        device, layout_.canvas_w, layout_.canvas_h, DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_BIND_SHADER_RESOURCE, border_bgra.data,
        static_cast<UINT>(layout_.canvas_w * 4));
    if (!border_tex) {
        if (error) *error = "failed to create border texture";
        return false;
    }
    border_srv_ = create_srv(device, border_tex.Get());
    if (!border_srv_) {
        if (error) *error = "failed to create border SRV";
        return false;
    }

    const auto lut = build_lut(layout_, key, options_);
    const auto table = build_src_from_dst(layout_);
    auto lut_buf = create_structured_buffer(device, lut.size() * sizeof(uint32_t),
                                            sizeof(uint32_t), lut.data());
    auto table_buf = create_structured_buffer(device, table.size() * sizeof(uint32_t),
                                              sizeof(uint32_t), table.data());
    if (!lut_buf || !table_buf) {
        if (error) *error = "failed to create LUT buffers";
        return false;
    }
    lut_srv_ = create_buffer_srv(device, lut_buf.Get(), static_cast<UINT>(lut.size()));
    table_srv_ = create_buffer_srv(device, table_buf.Get(), static_cast<UINT>(table.size()));
    if (!lut_srv_ || !table_srv_) {
        if (error) *error = "failed to create LUT SRVs";
        return false;
    }

    ShaderParams params{};
    params.canvasW = static_cast<uint32_t>(layout_.canvas_w);
    params.canvasH = static_cast<uint32_t>(layout_.canvas_h);
    params.contentX = static_cast<uint32_t>(layout_.content_x);
    params.contentY = static_cast<uint32_t>(layout_.content_y);
    params.contentW = static_cast<uint32_t>(layout_.content_w);
    params.contentH = static_cast<uint32_t>(layout_.content_h);
    params.cell = static_cast<uint32_t>(layout_.cell);
    params.guard = static_cast<uint32_t>(layout_.guard);
    params.cols = static_cast<uint32_t>(layout_.cols());
    params.rows = static_cast<uint32_t>(layout_.rows());
    params.inner = static_cast<uint32_t>(layout_.inner());
    params.refW = static_cast<uint32_t>(layout_.ref_w());
    params.refH = static_cast<uint32_t>(layout_.ref_h());
    params.extraScale = static_cast<float>(extra_scale_);
    cb_ = create_constant_buffer(device, sizeof(params), &params);
    if (!cb_) {
        if (error) *error = "failed to create encode constant buffer";
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
        if (error) *error = "failed to create encode sampler";
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc, rasterizer_.GetAddressOf()))) {
        if (error) *error = "failed to create encode rasterizer";
        return false;
    }

    output_tex_ = create_texture(device, layout_.canvas_w, layout_.canvas_h,
                                 DXGI_FORMAT_B8G8R8A8_UNORM,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                 nullptr, 0);
    if (!output_tex_ ||
        FAILED(device->CreateRenderTargetView(output_tex_.Get(), nullptr,
                                              output_rtv_.GetAddressOf()))) {
        if (error) *error = "failed to create encode output texture";
        return false;
    }
    output_srv_ = create_srv(device, output_tex_.Get());
    if (!output_srv_) {
        if (error) *error = "failed to create encode output SRV";
        return false;
    }

    const uint32_t black = 0xFF000000u;
    auto fallback = create_texture(device, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, &black, sizeof(black));
    if (!fallback) {
        if (error) *error = "failed to create fallback texture";
        return false;
    }
    fallback_srv_ = create_srv(device, fallback.Get());
    if (!fallback_srv_) {
        if (error) *error = "failed to create fallback SRV";
        return false;
    }
    return true;
}

void EncodePipeline::render(ID3D11ShaderResourceView* source) {
    if (!context_ || !vs_ || !ps_) return;
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(layout_.canvas_w);
    vp.Height = static_cast<float>(layout_.canvas_h);
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[4] = {
        lut_srv_.Get(),
        table_srv_.Get(),
        source ? source : fallback_srv_.Get(),
        border_srv_.Get(),
    };
    ID3D11Buffer* cbs[1] = {cb_.Get()};
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};

    context_->OMSetRenderTargets(1, output_rtv_.GetAddressOf(), nullptr);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterizer_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, cbs);
    context_->PSSetShaderResources(0, 4, srvs);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[4] = {nullptr, nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 4, nulls);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->Flush();
}

}  // namespace obs
