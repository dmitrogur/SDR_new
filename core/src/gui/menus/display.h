#pragma once

namespace displaymenu {
    void init();
    void checkKeybinds();
    void draw(void* ctx);
    bool getWaterfall();
    void setWaterfall(bool waterfall); 
    bool getFFTHold();
    void setFFTHold(bool ffthold); 
}