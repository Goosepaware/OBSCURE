#include "live/d3d_helpers.h"

#include <d3dcompiler.h>

#include <stdexcept>

namespace obs {

bool create_device(ComPtr<ID3D11Device>* device,
                   ComPtr<ID3D11DeviceContext>* context, std::string* error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 2, D3D11_SDK_VERSION, device->GetAddressOf(),
                                   &level, context->GetAddressOf());
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               2, D3D11_SDK_VERSION, device->GetAddressOf(), &level,
                               context->GetAddressOf());
    }
    if (FAILED(hr)) {
        if (error) *error = "D3D11CreateDevice failed";
        return false;
    }
    return true;
}

ComPtr<ID3DBlob> compile_shader(const std::wstring& path, const char* entry,
                                const char* target, std::string* error) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompileFromFile(
        path.c_str(), nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) {
            if (error) {
                *error = std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize());
            }
        } else if (error) {
            *error = "D3DCompileFromFile failed";
        }
        return nullptr;
    }
    return blob;
}

ComPtr<ID3D11Texture2D> create_texture(ID3D11Device* device, int width, int height,
                                       DXGI_FORMAT format, UINT bind_flags,
                                       const void* initial, UINT pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    D3D11_SUBRESOURCE_DATA data{};
    if (initial) {
        data.pSysMem = initial;
        data.SysMemPitch = pitch;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, initial ? &data : nullptr,
                                       texture.GetAddressOf()))) {
        return nullptr;
    }
    return texture;
}

ComPtr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, int width, int height,
                                               DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()))) {
        return nullptr;
    }
    return texture;
}

ComPtr<ID3D11ShaderResourceView> create_srv(ID3D11Device* device,
                                            ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture, &srv, view.GetAddressOf()))) {
        return nullptr;
    }
    return view;
}

ComPtr<ID3D11Buffer> create_constant_buffer(ID3D11Device* device, size_t bytes,
                                            const void* data) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(bytes);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&desc, &init, buffer.GetAddressOf()))) {
        return nullptr;
    }
    return buffer;
}

ComPtr<ID3D11Buffer> create_structured_buffer(ID3D11Device* device, size_t bytes,
                                              UINT stride, const void* data) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(bytes);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&desc, &init, buffer.GetAddressOf()))) {
        return nullptr;
    }
    return buffer;
}

ComPtr<ID3D11ShaderResourceView> create_buffer_srv(ID3D11Device* device,
                                                   ID3D11Buffer* buffer,
                                                   UINT elements) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = elements;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(buffer, &srv, view.GetAddressOf()))) {
        return nullptr;
    }
    return view;
}

}  // namespace obs
