#pragma once

#include <d3d11.h>

#include <memory>
#include <string>

namespace obs {

class SpoutSink {
public:
    SpoutSink();
    ~SpoutSink();
    SpoutSink(const SpoutSink&) = delete;
    SpoutSink& operator=(const SpoutSink&) = delete;

    bool start(ID3D11Device* device, const std::string& name, std::string* error);
    void stop();
    void send(ID3D11Texture2D* texture);
    bool initialized() const;
    const std::string& name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace obs
