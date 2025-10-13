#include <gui/icons.h>
#include <stdint.h>
#include <config.h>

#define STB_IMAGE_IMPLEMENTATION
#include <imgui/stb_image.h>
#include <filesystem>
#include <utils/flog.h>

namespace icons {
    ImTextureID LOGO;
    ImTextureID PLAY;
    ImTextureID STOP;
    ImTextureID MENU;
    ImTextureID MUTED;
    ImTextureID UNMUTED;
    ImTextureID NORMAL_TUNING;
    ImTextureID CENTER_TUNING;
    ImTextureID SHOW;
    ImTextureID WATERFALL;
    ImTextureID FFTHOLD;
    ImTextureID NET_ERROR;
    ImTextureID NET_STATUS;
    ImTextureID NET_HUB;
    ImTextureID NET_OK;
    ImTextureID NET_STATUS1;
    ImTextureID NET_HUB1;
    ImTextureID NET_STATUS2;
    ImTextureID NET_HUB2;
    ImTextureID NET_STATUS3;
    ImTextureID NET_HUB3;
    ImTextureID NET_STATUS4;
    ImTextureID NET_HUB4;
    ImTextureID NET_STATUS5;
    ImTextureID NET_HUB5;
    ImTextureID NET_STATUS6;
    ImTextureID NET_HUB6;
    ImTextureID NET_STATUS7;
    ImTextureID NET_HUB7;
    ImTextureID NET_STATUS8;
    ImTextureID NET_HUB8;

    GLuint loadTexture(std::string path) {
        int w, h, n;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 0);
        GLuint texId;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t*)data);
        stbi_image_free(data);
        return texId;
    }

    bool load(std::string resDir) {
        if (!std::filesystem::is_directory(resDir)) {
            flog::error("Invalid resource directory: {0}", resDir);
            return false;
        }

        LOGO = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/aster.png");
        PLAY = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/play.png");
        STOP = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/stop.png");
        MENU = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/menu.png");
        SHOW = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/show.png");
        MUTED = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/muted.png");
        UNMUTED = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/unmuted.png");
        NORMAL_TUNING = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/normal_tuning.png");
        CENTER_TUNING = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/center_tuning.png");
        WATERFALL = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/waterfall.png");
        FFTHOLD = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/ffthold.png");
        NET_ERROR = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_error.png");
        NET_STATUS = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_OK = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_ok.png");
        NET_HUB1 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS1 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB2 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS2 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB3 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS3 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB4 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS4 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB5 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS5 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB6 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS6 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB7 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS7 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");
        NET_HUB8 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_hub.png");
        NET_STATUS8 = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/net_status.png");


        return true;
    }
}