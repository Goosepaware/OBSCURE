#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "core/key.h"
#include "core/layout.h"

namespace obs {

class DecodePipeline {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, const Layout& layout,
              const KeyMaterial& key, const std::wstring& shader_path, std::string* error);
    void set_margin(int margin);
    void render(ID3D11ShaderResourceView* capture, int capture_w, int capture_h,
                const cv::Mat& H);

    ID3D11Texture2D* restored_texture() const { return restored_tex_.Get(); }
    ID3D11ShaderResourceView* restored_srv() const { return restored_srv_.Get(); }
    const Layout& layout() const { return layout_; }
    int margin() const { return margin_; }

private:
    struct MapEntry {
        float x;
        float y;
    };

    Layout layout_;
    int margin_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lut_srv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> map_buf_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> map_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> restored_tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> restored_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> restored_srv_;
};

}  // namespace obs
