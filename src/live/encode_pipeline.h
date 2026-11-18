#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"

namespace obs {

class EncodePipeline {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, const Layout& layout,
              const KeyMaterial& key, double extra_scale,
              const TransportOptions& options,
              const std::wstring& shader_path, std::string* error);
    void render(ID3D11ShaderResourceView* source);

    ID3D11Texture2D* output_texture() const { return output_tex_.Get(); }
    ID3D11ShaderResourceView* output_srv() const { return output_srv_.Get(); }
    const Layout& layout() const { return layout_; }
    double extra_scale() const { return extra_scale_; }
    const TransportOptions& options() const { return options_; }

private:
    void upload_params();

    Layout layout_;
    double extra_scale_ = 1.0;
    TransportOptions options_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> border_srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lut_srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> table_srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fallback_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_;
};

}  // namespace obs
