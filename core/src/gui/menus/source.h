#pragma once

#include <gui/gui.h>

namespace sourcemenu {
    void init();
    void draw(void* ctx);
    int getSourceId();
    std::string getCurrSource();
}