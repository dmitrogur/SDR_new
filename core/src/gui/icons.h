#pragma once
#include <imgui/imgui.h>
#include <string>

#include <utils/opengl_include_code.h>

namespace icons {
    extern ImTextureID LOGO;
    extern ImTextureID PLAY;
    extern ImTextureID STOP;
    extern ImTextureID MENU;
    extern ImTextureID SHOW;
    extern ImTextureID MUTED;
    extern ImTextureID UNMUTED;
    extern ImTextureID NORMAL_TUNING;
    extern ImTextureID CENTER_TUNING;
    extern ImTextureID WATERFALL;
    extern ImTextureID FFTHOLD;
    extern ImTextureID NET_ERROR;
    extern ImTextureID NET_STATUS;
    extern ImTextureID NET_HUB;
    extern ImTextureID NET_OK;
    extern ImTextureID NET_STATUS1;
    extern ImTextureID NET_HUB1;
    extern ImTextureID NET_STATUS2;
    extern ImTextureID NET_HUB2;
    extern ImTextureID NET_STATUS3;
    extern ImTextureID NET_HUB3;
    extern ImTextureID NET_STATUS4;
    extern ImTextureID NET_HUB4;
    extern ImTextureID NET_STATUS5;
    extern ImTextureID NET_HUB5;
    extern ImTextureID NET_STATUS6;
    extern ImTextureID NET_HUB6;
    extern ImTextureID NET_STATUS7;
    extern ImTextureID NET_HUB7;
    extern ImTextureID NET_STATUS8;
    extern ImTextureID NET_HUB8;
    GLuint loadTexture(std::string path);
    bool load(std::string resDir);
}