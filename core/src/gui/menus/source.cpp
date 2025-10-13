#include <gui/menus/source.h>
#include <imgui.h>
// #include <gui/gui.h>
#include <core.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>

namespace sourcemenu {
    int offsetMode = 0;
    int sourceId = 0;
    double customOffset = 0.0;
    double effectiveOffset = 0.0;
    int decimationPower = 0;
    bool iqCorrection = true;
    bool invertIQ = false;
    std::string sourceName = "File";

    EventHandler<std::string> sourceRegisteredHandler;
    EventHandler<std::string> sourceUnregisterHandler;
    EventHandler<std::string> sourceUnregisteredHandler;

    std::vector<std::string> sourceNames;
    std::string sourceNamesTxt;
    std::string selectedSource;

    enum {
        OFFSET_MODE_NONE,
        OFFSET_MODE_CUSTOM,
        OFFSET_MODE_SPYVERTER,
        OFFSET_MODE_HAM_IT_UP,
        OFFSET_MODE_MMDS_SB_1998,
        OFFSET_MODE_DK5AV_XB,
        OFFSET_MODE_KU_LNB_9750,
        OFFSET_MODE_KU_LNB_10700,
        _OFFSET_MODE_COUNT
    };

    const char* offsetModesTxt = "Вимк.\0"
                                 "Custom\0"
                                 "SpyVerter\0"
                                 "Ham-It-Up\0"
                                 "MMDS S-band (1998МГц)\0"
                                 "DK5AV X-Band\0"
                                 "Ku LNB (9750МГц)\0"
                                 "Ku LNB (10700МГц)\0";

    const char* decimationStages = "Вимк.\0"
                                   "2\0"
                                   "4\0"
                                   "8\0"
                                   "16\0"
                                   "32\0"
                                   "64\0"
                                   "128\0"
                                   "256\0";

    void updateOffset() {
        if (offsetMode == OFFSET_MODE_CUSTOM) { effectiveOffset = customOffset; }
        else if (offsetMode == OFFSET_MODE_SPYVERTER) {
            effectiveOffset = 120000000;
        } // 120MHz Up-conversion
        else if (offsetMode == OFFSET_MODE_HAM_IT_UP) {
            effectiveOffset = 125000000;
        } // 125MHz Up-conversion
        else if (offsetMode == OFFSET_MODE_MMDS_SB_1998) {
            effectiveOffset = -1998000000;
        } // 1.998GHz Down-conversion
        else if (offsetMode == OFFSET_MODE_DK5AV_XB) {
            effectiveOffset = -6800000000;
        } // 6.8GHz Down-conversion
        else if (offsetMode == OFFSET_MODE_KU_LNB_9750) {
            effectiveOffset = -9750000000;
        } // 9.750GHz Down-conversion
        else if (offsetMode == OFFSET_MODE_KU_LNB_10700) {
            effectiveOffset = -10700000000;
        } // 10.7GHz Down-conversion
        else {
            effectiveOffset = 0;
        }
        sigpath::sourceManager.setTuningOffset(effectiveOffset);
    }

    void refreshSources() {
        sourceNames = sigpath::sourceManager.getSourceNames();
        sourceNamesTxt.clear();
        for (auto name : sourceNames) {
            sourceNamesTxt += name;
            sourceNamesTxt += '\0';
            flog::info("  TRACE sourceId {0}:{1} refreshSources", sourceId, sourceNamesTxt);
        }
    }

    std::string getCurrSource() {
        return sourceNames[sourceId];
    }

    int getSourceId() {
        return sourceId;
    }

    void selectSource(std::string name) {
        if (sourceNames.empty()) {
            selectedSource.clear();
            return;
        }
        auto it = std::find(sourceNames.begin(), sourceNames.end(), name);
        if (it == sourceNames.end()) {
            selectSource(sourceNames[0]);
            return;
        }
        sourceId = std::distance(sourceNames.begin(), it);
        selectedSource = sourceNames[sourceId];
        flog::info("TRACE sourceId {0}:{1} selectSource", sourceId, sourceNames[sourceId]);
        sigpath::sourceManager.selectSource(sourceNames[sourceId]);
    }

    void onSourceRegistered(std::string name, void* ctx) {
        refreshSources();

        if (selectedSource.empty()) {
            sourceId = 0;
            sourceId = 0;
            selectSource(sourceNames[0]);

            return;
        }

        sourceId = std::distance(sourceNames.begin(), std::find(sourceNames.begin(), sourceNames.end(), selectedSource));
    }

    void onSourceUnregister(std::string name, void* ctx) {
        if (name != selectedSource) { return; }

        // TODO: Stop everything
    }

    void onSourceUnregistered(std::string name, void* ctx) {
        refreshSources();

        if (sourceNames.empty()) {
            selectedSource = "";
            return;
        }

        if (name == selectedSource) {
            sourceId = std::clamp<int>(sourceId, 0, sourceNames.size() - 1);
            flog::info("TRACE sourceId {0}:{1} onSourceUnregistered", sourceId, sourceNames[sourceId]);
            selectSource(sourceNames[sourceId]);
            return;
        }

        sourceId = std::distance(sourceNames.begin(), std::find(sourceNames.begin(), sourceNames.end(), selectedSource));
    }

    void init() {
        // Define decimation values
        /*
        decimations.define(1, "None", 1);
        decimations.define(2, "2x", 2);
        decimations.define(4, "4x", 4);
        decimations.define(8, "8x", 8);
        decimations.define(16, "16x", 16);
        decimations.define(32, "32x", 32);
        decimations.define(64, "64x", 64);
        */

        core::configManager.acquire();
        sourceName = core::configManager.conf["source"];
        customOffset = core::configManager.conf["offset"];
        offsetMode = core::configManager.conf["offsetMode"];
        iqCorrection = true; // core::configManager.conf["iqCorrection"];
        invertIQ = core::configManager.conf["invertIQ"];
        sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
        updateOffset();
        refreshSources();
        flog::info("TRACE sourceId {0}: selected = {1} init", sourceId, sourceName);
        selectSource(sourceName);
        if (sourceName == "Файл" || sourceName == "File") {
            sigpath::iqFrontEnd.setInvertIQ(invertIQ);
            core::configManager.conf["centerTuning"] = true; // tuner::TUNER_MODE_NORMAL;
        }
        else {
            invertIQ = false;
            sigpath::iqFrontEnd.setInvertIQ(invertIQ);
        }
        if (core::configManager.conf.contains("decimationPower") && !core::configManager.conf["decimationPower"].is_null()) {
            decimationPower = core::configManager.conf["decimationPower"];
        }
        else {
            core::configManager.conf["decimationPower"] = 1;
        }
        core::configManager.release(true);

        sigpath::iqFrontEnd.setDecimation(1 << decimationPower);
        sourceRegisteredHandler.handler = onSourceRegistered;
        sourceUnregisterHandler.handler = onSourceUnregister;
        sourceUnregisteredHandler.handler = onSourceUnregistered;
        sigpath::sourceManager.onSourceRegistered.bindHandler(&sourceRegisteredHandler);
        sigpath::sourceManager.onSourceUnregister.bindHandler(&sourceUnregisterHandler);
        sigpath::sourceManager.onSourceUnregistered.bindHandler(&sourceUnregisteredHandler);
    }

    void draw(void* ctx) {
        float itemWidth = ImGui::GetContentRegionAvail().x;
        bool running = gui::mainWindow.isPlaying();
        // DMH -----
        {
            if (running) { style::beginDisabled(); }
            ImGui::SetNextItemWidth(itemWidth);
            if (ImGui::Combo("##source", &sourceId, sourceNamesTxt.c_str())) {
                flog::info("TRACE DMH sourceId {0}: sourceName = {1} Combo", sourceId, sourceNames[sourceId]);
                selectSource(sourceNames[sourceId]);
                flog::info("TRACE sourceId DMH {0}: sourceName = {1} Combo", sourceId, sourceNames[sourceId]);
                sourceName = sourceNames[sourceId];
                core::configManager.acquire();
                core::configManager.conf["source"] = sourceName;
                core::configManager.release(true);

                double def_freq = core::configManager.conf["frequency"];

                gui::mainWindow.setSource(sourceName);

                if (sourceName == "Файл" || sourceName == "File") {
                    invertIQ = false;
                    sigpath::iqFrontEnd.setInvertIQ(invertIQ);
                    core::configManager.acquire();
                    core::configManager.conf["centerTuning"] = false; // tuner::TUNER_MODE_NORMAL;
                    core::configManager.release(true);
                    gui::waterfall.setCenterFrequency(def_freq);
                    gui::waterfall.centerFreqMoved = true;
                }
                else {
                    gui::waterfall.setCenterFrequency(def_freq);
                    gui::waterfall.centerFreqMoved = true;
                }
            }
            if (running) { style::endDisabled(); }
        }
        sigpath::sourceManager.showSelectedMenu();
        /*
        if (ImGui::Checkbox("Корекція IQ##_sdrpp_iq_corr", &iqCorrection)) {
            sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
            core::configManager.acquire();
            core::configManager.conf["iqCorrection"] = iqCorrection;
            core::configManager.release(true);
        }
        */
        if (sourceName == "Файл" || sourceName == "File") {
            if (ImGui::Checkbox("Інвертувати IQ##_sdrpp_inv_iq", &invertIQ)) {
                sigpath::iqFrontEnd.setInvertIQ(invertIQ);
                core::configManager.acquire();
                core::configManager.conf["invertIQ"] = invertIQ;
                core::configManager.release(true);
            }
        }
        /*
        ImGui::LeftLabel("Offset mode");
        ImGui::SetNextItemWidth(itemWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_sdrpp_offset_mode", &offsetMode, offsetModesTxt)) {
            updateOffset();
            core::configManager.acquire();
            core::configManager.conf["offsetMode"] = offsetMode;
            core::configManager.release(true);
        }

        ImGui::LeftLabel("Offset");
        ImGui::SetNextItemWidth(itemWidth - ImGui::GetCursorPosX());
        if (offsetMode == OFFSET_MODE_CUSTOM) {
            if (ImGui::InputDouble("##freq_offset", &customOffset, 1.0, 100.0)) {
                updateOffset();
                core::configManager.acquire();
                core::configManager.conf["offset"] = customOffset;
                core::configManager.release(true);
            }
        }
        else {
            style::beginDisabled();
            ImGui::InputDouble("##freq_offset", &effectiveOffset, 1.0, 100.0);
            style::endDisabled();
        }
        */
        if (running) { style::beginDisabled(); }
        ImGui::LeftLabel("Децимація");
        ImGui::SetNextItemWidth(itemWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##source_decim", &decimationPower, decimationStages)) {
            sigpath::iqFrontEnd.setDecimation(1 << decimationPower);
            core::configManager.acquire();
            core::configManager.conf["decimationPower"] = decimationPower;
            core::configManager.release(true);
        }
        if (running) { style::endDisabled(); }
        
    }
}
