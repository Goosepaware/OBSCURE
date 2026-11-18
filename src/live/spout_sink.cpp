#include "live/spout_sink.h"

#include "SpoutDX.h"

namespace obs {

struct SpoutSink::Impl {
    spoutDX sender;
    bool open = false;
    std::string name = "OBScure";
};

SpoutSink::SpoutSink() : impl_(std::make_unique<Impl>()) {}

SpoutSink::~SpoutSink() {
    stop();
}

bool SpoutSink::start(ID3D11Device* device, const std::string& name, std::string* error) {
    stop();
    if (!device) {
        if (error) *error = "Spout: null D3D device";
        return false;
    }
    if (!impl_->sender.OpenDirectX11(device)) {
        if (error) *error = "Spout: OpenDirectX11 failed";
        return false;
    }
    impl_->sender.SetSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
    impl_->name = name.empty() ? "OBScure" : name;
    impl_->sender.SetSenderName(impl_->name.c_str());
    impl_->open = true;
    return true;
}

void SpoutSink::stop() {
    if (!impl_->open) return;
    impl_->sender.ReleaseSender();
    impl_->open = false;
}

void SpoutSink::send(ID3D11Texture2D* texture) {
    if (!impl_->open || !texture) return;
    impl_->sender.SendTexture(texture);
}

bool SpoutSink::initialized() const {
    return impl_->open && impl_->sender.IsInitialized();
}

const std::string& SpoutSink::name() const {
    return impl_->name;
}

}  // namespace obs
