#include <windows.h>

#include "gui/gui_app.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (HWND console = GetConsoleWindow()) {
        ShowWindow(console, SW_HIDE);
    }
    FreeConsole();
    obs::GuiApp app;
    return app.run();
}
