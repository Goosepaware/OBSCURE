#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace obs {

using Microsoft::WRL::ComPtr;

bool create_device(ComPtr<ID3D11Device>* device,
                   ComPtr<ID3D11DeviceContext>* context, std::string* error);

ComPtr<ID3DBlob> compile_shader(const std::wstring& path, const char* entry,
                                const char* target, std::string* error);

ComPtr<ID3D11Texture2D> create_texture(ID3D11Device* device, int width, int height,
                                       DXGI_FORMAT format, UINT bind_flags,
                                       const void* initial, UINT pitch);

ComPtr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, int width, int height,
                                               DXGI_FORMAT format);

ComPtr<ID3D11ShaderResourceView> create_srv(ID3D11Device* device,
                                            ID3D11Texture2D* texture);

ComPtr<ID3D11Buffer> create_constant_buffer(ID3D11Device* device, size_t bytes,
                                            const void* data);

ComPtr<ID3D11Buffer> create_structured_buffer(ID3D11Device* device, size_t bytes,
                                              UINT stride, const void* data);

ComPtr<ID3D11ShaderResourceView> create_buffer_srv(ID3D11Device* device,
                                                   ID3D11Buffer* buffer,
                                                   UINT elements);

}  // namespace obs
