#pragma once

#include <memory>

namespace obs {

class GuiApp {
public:
    GuiApp();
    ~GuiApp();
    GuiApp(const GuiApp&) = delete;
    GuiApp& operator=(const GuiApp&) = delete;

    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace obs
