#include <gui/widgets/waterfall.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imutils.h>
#include <algorithm>
#include <volk/volk.h>
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include "imgui.h"
#include <gui/menus/source.h>
#include <gui/icons.h>
#include <gui/main_window.h>
#include <gui/menus/display.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <vector>
#include <cmath> // для round
#include <iomanip>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <locale>
#include <ctime>
#include <signal_path/signal_path.h>

#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;

float DEFAULT_COLOR_MAP[][3] = {
    {0x00, 0x00, 0x20},
    {0x00, 0x00, 0x30},
    {0x00, 0x00, 0x50},
    {0x00, 0x00, 0x91},
    {0x1E, 0x90, 0xFF},
    {0xFF, 0xFF, 0xFF},
    {0xFF, 0xFF, 0x00},
    {0xFE, 0x6D, 0x16},
    {0xFF, 0x00, 0x00},
    {0xC6, 0x00, 0x00},
    {0x9F, 0x00, 0x00},
    {0x75, 0x00, 0x00},
    {0x4A, 0x00, 0x00}};

// TODO: Fix this hacky BS

double freq_ranges[] = {
    1.0, 2.0, 2.5, 5.0,
    10.0, 20.0, 25.0, 50.0,
    100.0, 200.0, 250.0, 500.0,
    1000.0, 2000.0, 2500.0, 5000.0,
    10000.0, 20000.0, 25000.0, 50000.0,
    100000.0, 200000.0, 250000.0, 500000.0,
    1000000.0, 2000000.0, 2500000.0, 5000000.0,
    10000000.0, 20000000.0, 25000000.0, 50000000.0};

const char *demodModeListEngl[] = {
    "NFM",
    "WFM",
    "AM",
    "DSB",
    "USB",
    "CW",
    "LSB",
    "RAW"};

const char *demodModeList[] = {
    "ЧМ",
    "ЧМ-Ш",
    "AM",
    "ПБС",
    "ВБС",
    "HC",
    "НБС",
    "СМО"};

const char *demodModeListFile[] = {
    "ЧМ",
    "ЧМШ",
    "AM",
    "ПБС",
    "ВБС",
    "HC",
    "НБС",
    "СМО"};
#define SIZE_BOT_PANEL 270
#define SIZE_BOT_PANEL_ARM 150

inline double findBestRange(double bandwidth, int maxSteps)
{
    for (int i = 0; i < 32; i++)
    {
        if (bandwidth / freq_ranges[i] < (double)maxSteps)
        {
            return freq_ranges[i];
        }
    }
    return 50000000.0;
}

inline void printAndScale(double freq, char *buf)
{
    double freqAbs = fabs(freq);
    if (freqAbs < 1000)
    {
        sprintf(buf, "%.6g", freq);
    }
    else if (freqAbs < 1000000)
    {
        sprintf(buf, "%.6lgK", freq / 1000.0);
    }
    else if (freqAbs < 1000000000)
    {
        sprintf(buf, "%.6lgM", freq / 1000000.0);
    }
    else if (freqAbs < 1000000000000)
    {
        sprintf(buf, "%.6lgG", freq / 1000000000.0);
    }
}

inline void doZoom(int offset, int width, int inSize, int outSize, float *in, float *out)
{
    // NOTE: REMOVE THAT SHIT, IT'S JUST A HACKY FIX
    if (offset < 0)
    {
        offset = 0;
    }
    if (width > 524288)
    {
        width = 524288;
    }

    float factor = (float)width / (float)outSize;
    float sFactor = ceilf(factor);
    float uFactor;
    float id = offset;
    float maxVal;
    int sId;
    for (int i = 0; i < outSize; i++)
    {
        maxVal = -INFINITY;
        sId = (int)id;
        uFactor = (sId + sFactor > inSize) ? sFactor - ((sId + sFactor) - inSize) : sFactor;
        for (int j = 0; j < uFactor; j++)
        {
            if (in[sId + j] > maxVal)
            {
                maxVal = in[sId + j];
            }
        }
        out[i] = maxVal;
        id += factor;
    }
}

namespace ImGui
{
    WaterFall::WaterFall()
    {
        fftMin = -20.0;
        fftMax = 0.0;
        waterfallMin = -20.0;
        waterfallMax = 0.0;
        FFTAreaHeight = 300;
        newFFTAreaHeight = FFTAreaHeight;
        fftHeight = FFTAreaHeight - 50; // 50; // 100;
        dataWidth = 600;                // 600;
        lastWidgetPos.x = 0;
        lastWidgetPos.y = 0;
        lastWidgetSize.x = 0;
        lastWidgetSize.y = 0;
        latestFFT = new float[dataWidth];
        latestFFTHold = new float[dataWidth];
        waterfallFb = new uint32_t[1];

        viewBandwidth = 1.0;
        wholeBandwidth = 1.0;

        updatePallette(DEFAULT_COLOR_MAP, 13);

        radioMode = 4;
        SERVERS_Count = 8;
        ModeTxt = "Ручний ";
        ModeTxt += '\0';
        ModeTxt += "Пошук ";
        ModeTxt += '\0';
        ModeTxt += "Сканування ";
        ModeTxt += '\0';
        ModeTxt += "Спостереження";

        // SearchListTxt = "General";
        // ScanListTxt = "General";
        // CTRLListTxt = "General";
        finded_freq.clear();
    }

    void WaterFall::init()
    {
        glGenTextures(1, &textureId);
        std::string pathValid = core::configManager.getPath() + "/Banks/*";
        std::string jsonPath = pathValid + "/black_list.json";
        core::configManager.acquire();
        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            radioMode = 4;
        }
        try
        {
            SERVERS_Count = core::configManager.conf["SERVERS_COUNT"];
        }
        catch (const std::exception &e)
        {
            SERVERS_Count = 8;
        }
        if (SERVERS_Count > MAX_SERVERS)
            SERVERS_Count = MAX_SERVERS;
        flog::info("radioMode={0}", radioMode);
        try
        {
            jsonPath = core::configManager.conf["PathJson"];
            flog::info("jsonPath={0}", jsonPath);
        }
        catch (const std::exception &e)
        {
            jsonPath = "/opt/banks/";
            std::cerr << e.what() << '\n';
        }

        const auto &receivers = core::configManager.conf["receivers"];

        for (int ch = 0; ch < MAX_SERVERS; ++ch)
        {
            if (ch >= receivers.size())
                continue;

            const auto &recv = receivers[ch];

            if (!recv.contains("mode") || recv["mode"].is_null())
                continue;

            int mode = recv["mode"];
            selectedMode[ch] = mode;
            flog::warn("[WF] selectedMode[{0}] {1}", ch, selectedMode[ch]);
            if (selectedMode[ch] > 0 && selectedMode[ch] < 5)
                gui::mainWindow.setSelectedMode(ch, selectedMode[ch]);

            // Проверка и установка bank, если mode = 1, 2, 3
            if ((mode == 1 || mode == 2 || mode == 3) &&
                recv.contains("bank") && !recv["bank"].is_null())
            {

                int bank = recv["bank"];

                switch (mode)
                {
                case 1:
                    searchListId[ch] = bank;
                    gui::mainWindow.setidOfList_srch(ch, bank);
                    break;
                case 2:
                    scanListId[ch] = bank;
                    gui::mainWindow.setidOfList_scan(ch, bank);
                    break;
                case 3:
                    ctrlListId[ch] = bank;
                    gui::mainWindow.setidOfList_ctrl(ch, bank);
                    break;
                }
            }
        }
        core::configManager.release();

        try
        {
            fs::create_directory(jsonPath);
            pathValid = jsonPath + "/Search";
            fs::create_directory(pathValid);
            flog::info("jsonPath={0}", pathValid);
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            pathValid = core::configManager.getPath() + "/Banks";
        }
        /*
        std::string expname = pathValid + "/black_list.json";
        if (std::filesystem::exists(expname))
        {
            flog::info("expname '{0}'", expname);
            importBookmarks(expname, true);
        }
        else
        {
            std::cout << "Файл не найден.\n";
        }
        */
        loadSearchBlacklistFromFile(pathValid);
    }

    void WaterFall::UpdateBlackList(const std::vector<SkipFoundBookmark> &items)
    {
        std::lock_guard<std::mutex> lock(skipFreqMutex);

        skip_finded_freq.clear();
        _count_Bookmark = 0;

        for (const auto &fbm : items)
        {
            double dfrec = fbm.frequency;
            skip_finded_freq[dfrec] = fbm;
            ++_count_Bookmark;
        }

        flog::info("UpdateBlackList: loaded {0} entries into skip_finded_freq", _count_Bookmark);
    }

    void WaterFall::loadSearchBlacklistFromFile(const std::string &dir)
    {
        std::string expname = dir + "/black_list.json";
        if (std::filesystem::exists(expname))
        {
            flog::info("expname '{0}'", expname);
            importBookmarks(expname, true); // как и было
        }
        else
        {
            std::cout << "Файл не найден.\n";
        }
    }

    void WaterFall::drawFFT()
    {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        // Calculate scaling factor
        float startLine = floorf(fftMax / vRange) * vRange;
        float vertRange = fftMax - fftMin;
        float scaleFactor = fftHeight / vertRange;
        char buf[100];

        ImU32 trace = ImGui::GetColorU32(ImGuiCol_PlotLines);
        ImU32 traceHold = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftHoldColor);
        ImU32 shadow = ImGui::GetColorU32(ImGuiCol_PlotLines, 0.2);
        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        float textVOffset = 10.0f * style::uiScale;

        // Vertical scale
        for (float line = startLine; line > fftMin; line -= vRange)
        {
            float yPos = fftAreaMax.y - ((line - fftMin) * scaleFactor);
            window->DrawList->AddLine(ImVec2(fftAreaMin.x, roundf(yPos)),
                                      ImVec2(fftAreaMax.x, roundf(yPos)),
                                      IM_COL32(50, 50, 50, 255), style::uiScale);
            sprintf(buf, "%d", (int)line);
            ImVec2 txtSz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(ImVec2(fftAreaMin.x - txtSz.x - textVOffset, roundf(yPos - (txtSz.y / 2.0))), text, buf);
        }

        // Horizontal scale
        double startFreq = ceilf(lowerFreq / range) * range;
        double horizScale = (double)dataWidth / viewBandwidth;
        float scaleVOfsset = 7 * style::uiScale;
        /// flog::info("printAndScale!!. startFreq =  {0}, upperFreq {1}, scaleVOfsset {2}, viewBandwidth {3}", startFreq, upperFreq, scaleVOfsset, viewBandwidth);
        for (double freq = startFreq; freq < upperFreq; freq += range)
        {
            double xPos = fftAreaMin.x + ((freq - lowerFreq) * horizScale);
            window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMin.y + 1),
                                      ImVec2(roundf(xPos), fftAreaMax.y),
                                      IM_COL32(50, 50, 50, 255), style::uiScale);
            window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMax.y),
                                      ImVec2(roundf(xPos), fftAreaMax.y + scaleVOfsset),
                                      text, style::uiScale);
            printAndScale(freq, buf);
            ImVec2 txtSz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(ImVec2(roundf(xPos - (txtSz.x / 2.0)), fftAreaMax.y + txtSz.y), text, buf);
        }

        // Data
        if (latestFFT != NULL && fftLines != 0)
        {
            for (int i = 1; i < dataWidth; i++)
            {
                double aPos = fftAreaMax.y - ((latestFFT[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFT[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), trace, 1.0);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i, roundf(bPos)),

                                          ImVec2(fftAreaMin.x + i, fftAreaMax.y), shadow, 1.0);
            }
        }

        // Hold
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0)
        {
            for (int i = 1; i < dataWidth; i++)
            {
                double aPos = fftAreaMax.y - ((latestFFTHold[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFTHold[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), traceHold, 1.0);
            }
        }

        FFTRedrawArgs args;
        args.min = fftAreaMin;
        args.max = fftAreaMax;
        args.lowFreq = lowerFreq;
        args.highFreq = upperFreq;
        args.freqToPixelRatio = horizScale;
        args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
        args.window = window;
        onFFTRedraw.emit(args);

        // X Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMax.y),
                                  ImVec2(fftAreaMax.x, fftAreaMax.y),
                                  text, style::uiScale);
        // Y Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMin.y),
                                  ImVec2(fftAreaMin.x, fftAreaMax.y - 1),
                                  text, style::uiScale);
    }

    void WaterFall::drawWaterfall()
    {
        if (waterfallUpdate)
        {
            waterfallUpdate = false;
            updateWaterfallTexture();
        }
        {
            std::lock_guard<std::mutex> lck(texMtx);
            window->DrawList->AddImage((void *)(intptr_t)textureId, wfMin, wfMax);
        }

        ImVec2 mPos = ImGui::GetMousePos();
        if (IS_IN_AREA(mPos, wfMin, wfMax) && !gui::mainWindow.lockWaterfallControls && !inputHandled)
        {
            for (auto const &[name, vfo] : vfos)
            {
                window->DrawList->AddRectFilled(vfo->wfRectMin, vfo->wfRectMax, vfo->color);
                if (!vfo->lineVisible)
                {
                    continue;
                }
                window->DrawList->AddLine(vfo->wfLineMin, vfo->wfLineMax, (name == selectedVFO) ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::uiScale);
            }
        }
    }

    void WaterFall::drawVFOs()
    {
        for (auto const &[name, vfo] : vfos)
        {
            vfo->draw(window, name == selectedVFO);
            // flog::info("void WaterFall::drawVFOs()  {0} - {1}", name,  selectedVFO);
        }
    }

    void WaterFall::selectFirstVFO()
    {
        bool available = false;
        std::string currSelectedVFO = "";
        for (auto const &[name, vfo] : vfos)
        {
            available = true;
            currSelectedVFO = name;
            if (currSelectedVFO == "Канал приймання")
            {
                selectedVFO = currSelectedVFO;
                // flog::info("WaterFall::selectFirstVFO() {0}", selectedVFO);
                selectedVFOChanged = true;
                return;
            }
        }
        if (!available)
        {
            selectedVFO = "";
            selectedVFOChanged = true;
        }
        else
        {
            selectedVFO = currSelectedVFO;
            // flog::info("WaterFall::selectFirstVFO() {0}", selectedVFO);
            selectedVFOChanged = true;
        }
    }

    bool WaterFall::setCurrVFO(std::string NewNameVFO)
    {
        // flog::info("setCurrVFO(std::string NewNameVFO) {0}", NewNameVFO);
        if (selectedVFO == NewNameVFO)
            return true;
        // flog::info("setCurrVFO(std::string NewNameVFO) {0}", NewNameVFO);
        bool available = false;
        std::string currSelectedVFO = "";
        for (auto const &[name, vfo] : vfos)
        {
            available = true;
            currSelectedVFO = name;
            if (currSelectedVFO == NewNameVFO)
            {
                selectedVFO = currSelectedVFO;
                // flog::info("!!!! WaterFall::selectFirstVFO() {0}", selectedVFO);
                selectedVFOChanged = true;
                return true;
            }
        }
        // flog::info(".... WaterFall::selectFirstVFO() {0}", selectedVFO);
        if (!available)
        {
            selectedVFO = "";
            selectedVFOChanged = true;
            return false;
        }
        else
        {
            return false;
        }
        return false;
    }

#define GET_PROCESSING 7

    void WaterFall::processInputs()
    {
        // Pre calculate useful values
        WaterfallVFO *selVfo = NULL;
        if (selectedVFO != "")
        {
            selVfo = vfos[selectedVFO];
        }
        /*
        int _work = false;
        core::modComManager.callInterface("Запис", GET_PROCESSING, NULL, &_work);
        if (sourcemenu::getCurrSource() == SOURCE_ARM) {
            bool button_srch = gui::mainWindow.getbutton_srch(gui::mainWindow.getCurrServer());
            bool button_scan = gui::mainWindow.getbutton_scan(gui::mainWindow.getCurrServer());
            bool button_ctrl = gui::mainWindow.getbutton_ctrl(gui::mainWindow.getCurrServer());
            bool recording = gui::mainWindow.getServerRecording(gui::mainWindow.getCurrServer());
            if (button_srch || button_scan || button_ctrl || recording) {
                _work = true;
            }
        }
        */
        if (gui::mainWindow.getIfOneButtonStart())
            return;
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        ImVec2 dragOrigin(mousePos.x - drag.x, mousePos.y - drag.y);

        bool mouseHovered, mouseHeld;
        bool mouseClicked = ImGui::ButtonBehavior(ImRect(fftAreaMin, wfMax), GetID("WaterfallID"), &mouseHovered, &mouseHeld,
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_PressedOnClick);

        mouseInFFTResize = (dragOrigin.x > widgetPos.x && dragOrigin.x < widgetPos.x + widgetSize.x && dragOrigin.y >= widgetPos.y + newFFTAreaHeight - (2.0f * style::uiScale) && dragOrigin.y <= widgetPos.y + newFFTAreaHeight + (2.0f * style::uiScale));
        mouseInFreq = IS_IN_AREA(dragOrigin, freqAreaMin, freqAreaMax);
        mouseInFFT = IS_IN_AREA(dragOrigin, fftAreaMin, fftAreaMax);
        mouseInWaterfall = IS_IN_AREA(dragOrigin, wfMin, wfMax);

        int mouseWheel = ImGui::GetIO().MouseWheel;

        bool mouseMoved = false;
        if (mousePos.x != lastMousePos.x || mousePos.y != lastMousePos.y)
        {
            mouseMoved = true;
        }
        lastMousePos = mousePos;

        std::string hoveredVFOName = "";
        for (auto const &[name, _vfo] : vfos)
        {
            if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax))
            {
                hoveredVFOName = name;
                break;
            }
        }

        // Deselect everything if the mouse is released
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (fftResizeSelect)
            {
                FFTAreaHeight = newFFTAreaHeight;
                onResize();
            }

            fftResizeSelect = false;
            freqScaleSelect = false;
            vfoSelect = false;
            vfoBorderSelect = false;
            lastDrag = 0;
        }

        bool targetFound = false;

        // If the mouse was clicked anywhere in the waterfall, check if the resize was clicked
        if (mouseInFFTResize)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                fftResizeSelect = true;
                targetFound = true;
            }
        }

        // If mouse was clicked inside the central part, check what was clicked

        if (mouseClicked && !targetFound)
        {
            mouseDownPos = mousePos;
            // First, check if a VFO border was selected
            for (auto const &[name, _vfo] : vfos)
            {
                if (_vfo->bandwidthLocked)
                {
                    continue;
                }
                if (_vfo->rectMax.x - _vfo->rectMin.x < 10)
                {
                    continue;
                }
                bool resizing = false;
                if (_vfo->reference != REF_LOWER)
                {
                    if (IS_IN_AREA(mousePos, _vfo->lbwSelMin, _vfo->lbwSelMax))
                    {
                        resizing = true;
                    }
                    else if (IS_IN_AREA(mousePos, _vfo->wfLbwSelMin, _vfo->wfLbwSelMax))
                    {
                        resizing = true;
                    }
                }
                if (_vfo->reference != REF_UPPER)
                {
                    if (IS_IN_AREA(mousePos, _vfo->rbwSelMin, _vfo->rbwSelMax))
                    {
                        resizing = true;
                    }
                    else if (IS_IN_AREA(mousePos, _vfo->wfRbwSelMin, _vfo->wfRbwSelMax))
                    {
                        resizing = true;
                    }
                }
                if (!resizing)
                {
                    continue;
                }
                relatedVfo = _vfo;
                vfoBorderSelect = true;
                targetFound = true;
                break;
            }

            if (!targetFound && hoveredVFOName != "" && hoveredVFOName == "Канал приймання")
            {
                selectedVFO = hoveredVFOName;
                selectedVFOChanged = true;
                targetFound = true;
                return;
            }

            // Now, check frequency scale
            if (!targetFound && mouseInFreq)
            {
                freqScaleSelect = true;
            }
        }

        // If the FFT resize bar was selected, resize FFT accordingly
        if (fftResizeSelect)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            newFFTAreaHeight = mousePos.y - widgetPos.y;
            newFFTAreaHeight = std::clamp<float>(newFFTAreaHeight, 150, widgetSize.y - 50);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(widgetPos.x, newFFTAreaHeight + widgetPos.y), ImVec2(widgetEndPos.x, newFFTAreaHeight + widgetPos.y),
                                                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), style::uiScale);
            return;
        }

        // If a vfo border is selected, resize VFO accordingly
        if (vfoBorderSelect)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double dist = (relatedVfo->reference == REF_CENTER) ? fabsf(mousePos.x - relatedVfo->lineMin.x) : (mousePos.x - relatedVfo->lineMin.x);
            if (relatedVfo->reference == REF_UPPER)
            {
                dist = -dist;
            }
            double hzDist = dist * (viewBandwidth / (double)dataWidth);
            if (relatedVfo->reference == REF_CENTER)
            {
                hzDist *= 2.0;
            }
            hzDist = std::clamp<double>(hzDist, relatedVfo->minBandwidth, relatedVfo->maxBandwidth);
            flog::info("set hzDist to setBandwidth");
            relatedVfo->setBandwidth(hzDist);
            relatedVfo->onUserChangedBandwidth.emit(hzDist);
            return;
        }

        // DMH--------------------------------------------
        // If the frequency scale is selected, move it
        if (freqScaleSelect && getSource() != "Файл" && getSource() != "Азалія-клієнт")
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double deltax = drag.x - lastDrag;
            lastDrag = drag.x;
            double viewDelta = deltax * (viewBandwidth / (double)dataWidth);

            viewOffset -= viewDelta;

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0)
            {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                if (!centerFrequencyLocked)
                {
                    setPrevCenterFrequency(centerFreq);
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0))
            {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                if (!centerFrequencyLocked)
                {
                    setPrevCenterFrequency(centerFreq);
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth)
            {
                updateAllVFOs();
                if (_fullUpdate)
                {
                    updateWaterfallFb();
                };
            }
            return;
        }
        // If the mouse wheel is moved on the frequency scale
        // DMH ---------------------------------
        if (mouseWheel != 0 && mouseInFreq)
        {
            viewOffset -= (double)mouseWheel * viewBandwidth / 20.0;

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0)
            {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                setPrevCenterFrequency(centerFreq);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0))
            {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                setPrevCenterFrequency(centerFreq);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth)
            {
                updateAllVFOs();
                if (_fullUpdate)
                {
                    updateWaterfallFb();
                };
            }
            return;
        }
        //-------------------------------- DMH

        // If the left and right keys are pressed while hovering the freq scale, move it too
        bool leftKeyPressed = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        if ((leftKeyPressed || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && mouseInFreq)
        {
            viewOffset += leftKeyPressed ? (viewBandwidth / 20.0) : (-viewBandwidth / 20.0);

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0)
            {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                setPrevCenterFrequency(centerFreq);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0))
            {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                setPrevCenterFrequency(centerFreq);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth)
            {
                updateAllVFOs();
                if (_fullUpdate)
                {
                    updateWaterfallFb();
                };
            }
            return;
        }

        // Finally, if nothing else was selected, just move the VFO
        if ((VFOMoveSingleClick ? ImGui::IsMouseClicked(ImGuiMouseButton_Left) : ImGui::IsMouseDown(ImGuiMouseButton_Left)) && (mouseInFFT | mouseInWaterfall) && (mouseMoved || hoveredVFOName == ""))
        {
            if (selVfo != NULL && selectedVFO == "Канал приймання")
            {
                int refCenter = mousePos.x - fftAreaMin.x;
                if (refCenter >= 0 && refCenter < dataWidth)
                {
                    double off = ((((double)refCenter / ((double)dataWidth / 2.0)) - 1.0) * (viewBandwidth / 2.0)) + viewOffset;
                    off += centerFreq;
                    off = (round(off / selVfo->snapInterval) * selVfo->snapInterval) - centerFreq;
                    selVfo->setOffset(off);
                }
            }
            else
            {
                flog::info("selectedVFO ({0}) !=  Канал приймання ", selectedVFO);
            }
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // Check if a VFO is hovered. If yes, show tooltip
            for (auto const &[name, _vfo] : vfos)
            {
                if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax))
                {
                    char buf[128];
                    ImGui::BeginTooltip();

                    ImGui::TextUnformatted(name.c_str());

                    if (ImGui::GetIO().KeyCtrl)
                    {
                        ImGui::Separator();
                        printAndScale(_vfo->generalOffset + centerFreq, buf);
                        ImGui::Text("Частота: %sГц", buf);
                        printAndScale(_vfo->bandwidth, buf);
                        ImGui::Text("Смуга: %sГц", buf);
                        ImGui::Text("Смуга заблокована: %s", _vfo->bandwidthLocked ? "Yes" : "No");

                        float strength, snr;
                        if (calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, _vfo, strength, snr))
                        {
                            ImGui::Text("Strength: %0.1fдБПШ", strength);
                            ImGui::Text("SNR: %0.1fдБ", snr);
                        }
                        else
                        {
                            ImGui::TextUnformatted("Strength: ---.-дБПШ");
                            ImGui::TextUnformatted("SNR: ---.-дБ");
                        }
                    }

                    ImGui::EndTooltip();
                    break;
                }
            }
        }

        // Handle Page Up to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && selVfo != NULL)
        {
            std::string next = (--vfos.end())->first;
            std::string lowest = "";
            double lowestOffset = INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto &[_name, _vfo] : vfos)
            {
                if (_vfo->generalOffset > firstVfoOffset && (_vfo->generalOffset - firstVfoOffset) < smallestDistance)
                {
                    next = _name;
                    smallestDistance = (_vfo->generalOffset - firstVfoOffset);
                    found = true;
                }
                if (_vfo->generalOffset < lowestOffset)
                {
                    lowestOffset = _vfo->generalOffset;
                    lowest = _name;
                }
            }
            selectedVFO = found ? next : lowest;
            flog::info("1 selectedVFO Page Up");

            selectedVFOChanged = true;
        }

        // Handle Page Down to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && selVfo != NULL)
        {
            std::string next = (--vfos.end())->first;
            std::string highest = "";
            double highestOffset = -INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto &[_name, _vfo] : vfos)
            {
                if (_vfo->generalOffset < firstVfoOffset && (firstVfoOffset - _vfo->generalOffset) < smallestDistance)
                {
                    next = _name;
                    smallestDistance = (firstVfoOffset - _vfo->generalOffset);
                    found = true;
                }
                if (_vfo->generalOffset > highestOffset)
                {
                    highestOffset = _vfo->generalOffset;
                    highest = _name;
                }
            }
            selectedVFO = found ? next : highest;
            flog::info("2 selectedVFO Page Down ");
            selectedVFOChanged = true;
        }
    }

    bool WaterFall::calculateVFOSignalInfo(float *fftLine, WaterfallVFO *_vfo, float &strength, float &snr)
    {
        if (fftLine == NULL || fftLines <= 0)
        {
            return false;
        }

        // Calculate FFT index data
        double vfoMinSizeFreq = _vfo->centerOffset - _vfo->bandwidth;
        double vfoMinFreq = _vfo->centerOffset - (_vfo->bandwidth / 2.0);
        double vfoMaxFreq = _vfo->centerOffset + (_vfo->bandwidth / 2.0);
        double vfoMaxSizeFreq = _vfo->centerOffset + _vfo->bandwidth;
        int vfoMinSideOffset = std::clamp<int>(((vfoMinSizeFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);
        int vfoMinOffset = std::clamp<int>(((vfoMinFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);
        int vfoMaxOffset = std::clamp<int>(((vfoMaxFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);
        int vfoMaxSideOffset = std::clamp<int>(((vfoMaxSizeFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);

        double avg = 0;
        float max = -INFINITY;
        int avgCount = 0;

        // Calculate Left average
        for (int i = vfoMinSideOffset; i < vfoMinOffset; i++)
        {
            avg += fftLine[i];
            avgCount++;
        }

        // Calculate Right average
        for (int i = vfoMaxOffset + 1; i < vfoMaxSideOffset; i++)
        {
            avg += fftLine[i];
            avgCount++;
        }

        avg /= (double)(avgCount);

        // Calculate max
        for (int i = vfoMinOffset; i <= vfoMaxOffset; i++)
        {
            if (fftLine[i] > max)
            {
                max = fftLine[i];
            }
        }

        strength = max;
        snr = max - avg;

        return true;
    }

    void WaterFall::updateWaterfallFb()
    {
        if (!waterfallVisible || rawFFTs == NULL)
        {
            return;
        }
        double offsetRatio = viewOffset / (wholeBandwidth / 2.0);
        int drawDataSize;
        int drawDataStart;
        // TODO: Maybe put on the stack for faster alloc?
        float *tempData = new float[dataWidth];
        float pixel;
        float dataRange = waterfallMax - waterfallMin;
        int count = std::min<float>(waterfallHeight, fftLines);
        if (rawFFTs != NULL && fftLines >= 0)
        {
            for (int i = 0; i < count; i++)
            {
                drawDataSize = (viewBandwidth / wholeBandwidth) * rawFFTSize;
                drawDataStart = (((double)rawFFTSize / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);
                // doZoom(drawDataStart, drawDataSize, dataWidth, &rawFFTs[((i + currentFFTLine) % waterfallHeight) * rawFFTSize], tempData);
                doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, &rawFFTs[((i + currentFFTLine) % waterfallHeight) * rawFFTSize], tempData);
                for (int j = 0; j < dataWidth; j++)
                {
                    pixel = (std::clamp<float>(tempData[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;
                    waterfallFb[(i * dataWidth) + j] = waterfallPallet[(int)(pixel * (WATERFALL_RESOLUTION - 1))];
                }
            }

            for (int i = count; i < waterfallHeight; i++)
            {
                for (int j = 0; j < dataWidth; j++)
                {
                    waterfallFb[(i * dataWidth) + j] = (uint32_t)255 << 24;
                }
            }
        }
        delete[] tempData;
        waterfallUpdate = true;
    }

    void WaterFall::drawBandPlan()
    {
        int count = bandplan->bands.size();
        double horizScale = (double)dataWidth / viewBandwidth;
        double start, end, center, aPos, bPos, cPos, width;
        ImVec2 txtSz;
        bool startVis, endVis;
        uint32_t color, colorTrans;

        float height = ImGui::CalcTextSize("0").y * 2.5f;
        float bpBottom;

        if (bandPlanPos == BANDPLAN_POS_BOTTOM)
        {
            bpBottom = fftAreaMax.y;
        }
        else
        {
            bpBottom = fftAreaMin.y + height + 1;
        }

        for (int i = 0; i < count; i++)
        {
            start = bandplan->bands[i].start;
            end = bandplan->bands[i].end;
            if (start < lowerFreq && end < lowerFreq)
            {
                continue;
            }
            if (start > upperFreq && end > upperFreq)
            {
                continue;
            }
            startVis = (start > lowerFreq);
            endVis = (end < upperFreq);
            start = std::clamp<double>(start, lowerFreq, upperFreq);
            end = std::clamp<double>(end, lowerFreq, upperFreq);
            center = (start + end) / 2.0;
            aPos = fftAreaMin.x + ((start - lowerFreq) * horizScale);
            bPos = fftAreaMin.x + ((end - lowerFreq) * horizScale);
            cPos = fftAreaMin.x + ((center - lowerFreq) * horizScale);
            width = bPos - aPos;
            txtSz = ImGui::CalcTextSize(bandplan->bands[i].name.c_str());
            if (bandplan::colorTable.find(bandplan->bands[i].type.c_str()) != bandplan::colorTable.end())
            {
                color = bandplan::colorTable[bandplan->bands[i].type].colorValue;
                colorTrans = bandplan::colorTable[bandplan->bands[i].type].transColorValue;
            }
            else
            {
                color = IM_COL32(255, 255, 255, 255);
                colorTrans = IM_COL32(255, 255, 255, 100);
            }
            if (aPos <= fftAreaMin.x)
            {
                aPos = fftAreaMin.x + 1;
            }
            if (bPos <= fftAreaMin.x)
            {
                bPos = fftAreaMin.x + 1;
            }
            if (width >= 1.0)
            {
                window->DrawList->AddRectFilled(ImVec2(roundf(aPos), bpBottom - height),
                                                ImVec2(roundf(bPos), bpBottom), colorTrans);
                if (startVis)
                {
                    window->DrawList->AddLine(ImVec2(roundf(aPos), bpBottom - height - 1),
                                              ImVec2(roundf(aPos), bpBottom - 1), color, style::uiScale);
                }
                if (endVis)
                {
                    window->DrawList->AddLine(ImVec2(roundf(bPos), bpBottom - height - 1),
                                              ImVec2(roundf(bPos), bpBottom - 1), color, style::uiScale);
                }
            }
            if (txtSz.x <= width)
            {
                window->DrawList->AddText(ImVec2(cPos - (txtSz.x / 2.0), bpBottom - (height / 2.0f) - (txtSz.y / 2.0f)),
                                          IM_COL32(255, 255, 255, 255), bandplan->bands[i].name.c_str());
            }
        }
    }

    void WaterFall::updateWaterfallTexture()
    {
        std::lock_guard<std::mutex> lck(texMtx);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dataWidth, waterfallHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t *)waterfallFb);
    }

    void WaterFall::onPositionChange()
    {
        // Nothing to see here...
    }

    void WaterFall::onResize()
    {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        std::lock_guard<std::mutex> lck2(smoothingBufMtx);
        // return if widget is too small
        if (widgetSize.x < 100 || widgetSize.y < 100)
        {
            return;
        }

        int lastWaterfallHeight = waterfallHeight;
        int SZ_PANEL = SIZE_BOT_PANEL;
        if (sourcemenu::getCurrSource() == SOURCE_ARM)
        {
            SZ_PANEL = SIZE_BOT_PANEL_ARM;
        }
        if (waterfallVisible)
        {
            FFTAreaHeight = std::min<int>(FFTAreaHeight, widgetSize.y - (50.0f * style::uiScale));
            newFFTAreaHeight = FFTAreaHeight;
            fftHeight = FFTAreaHeight - (50.0f * style::uiScale);
            waterfallHeight = widgetSize.y - fftHeight - (50.0f * style::uiScale) - 2 - SZ_PANEL;
            if (oldwaterfallVisible != waterfallVisible)
            {
                lastWaterfallHeight = waterfallHeight;
            }
        }
        else
        {
            if (oldwaterfallVisible != waterfallVisible)
            {
                waterfallHeight = 0;
                lastWaterfallHeight = 0;
            }
            fftHeight = widgetSize.y - (50.0f * style::uiScale) - SZ_PANEL;
        }
        oldwaterfallVisible = waterfallVisible;
        dataWidth = widgetSize.x - (60.0f * style::uiScale);
        // flog::info("onResize() fftHeight {0}, waterfallHeight {1}, lastWaterfallHeight {2};", fftHeight, waterfallHeight, lastWaterfallHeight);
        if (waterfallVisible)
        {
            // Raw FFT resize
            fftLines = std::min<int>(fftLines, waterfallHeight) - 1;
            if (rawFFTs != NULL)
            {
                if (currentFFTLine != 0)
                {
                    float *tempWF = new float[currentFFTLine * rawFFTSize];
                    int moveCount = lastWaterfallHeight - currentFFTLine;
                    memcpy(tempWF, rawFFTs, currentFFTLine * rawFFTSize * sizeof(float));
                    memmove(rawFFTs, &rawFFTs[currentFFTLine * rawFFTSize], moveCount * rawFFTSize * sizeof(float));
                    memcpy(&rawFFTs[moveCount * rawFFTSize], tempWF, currentFFTLine * rawFFTSize * sizeof(float));
                    delete[] tempWF;
                }
                currentFFTLine = 0;
                rawFFTs = (float *)realloc(rawFFTs, waterfallHeight * rawFFTSize * sizeof(float));
            }
            else
            {
                rawFFTs = (float *)malloc(waterfallHeight * rawFFTSize * sizeof(float));
            }
            // ==============
        }

        // Reallocate display FFT
        if (latestFFT != NULL)
        {
            delete[] latestFFT;
        }
        latestFFT = new float[dataWidth];

        // Reallocate hold FFT
        if (latestFFTHold != NULL)
        {
            delete[] latestFFTHold;
        }
        latestFFTHold = new float[dataWidth];

        // Reallocate smoothing buffer
        if (fftSmoothing)
        {
            if (smoothingBuf)
            {
                delete[] smoothingBuf;
            }
            smoothingBuf = new float[dataWidth];
            for (int i = 0; i < dataWidth; i++)
            {
                smoothingBuf[i] = -1000.0f;
            }
        }

        if (waterfallVisible)
        {
            delete[] waterfallFb;
            waterfallFb = new uint32_t[dataWidth * waterfallHeight];
            memset(waterfallFb, 0, dataWidth * waterfallHeight * sizeof(uint32_t));
        }
        for (int i = 0; i < dataWidth; i++)
        {
            latestFFT[i] = -1000.0f; // Hide everything
            latestFFTHold[i] = -1000.0f;
        }

        fftAreaMin = ImVec2(widgetPos.x + (50.0f * style::uiScale), widgetPos.y + (9.0f * style::uiScale));
        fftAreaMax = ImVec2(fftAreaMin.x + dataWidth, fftAreaMin.y + fftHeight + 1);

        freqAreaMin = ImVec2(fftAreaMin.x, fftAreaMax.y + 1);
        freqAreaMax = ImVec2(fftAreaMax.x, fftAreaMax.y + (40.0f * style::uiScale));

        wfMin = ImVec2(fftAreaMin.x, freqAreaMax.y + 1);
        wfMax = ImVec2(fftAreaMin.x + dataWidth, wfMin.y + waterfallHeight);

        maxHSteps = dataWidth / (ImGui::CalcTextSize("000.000").x + 10);
        maxVSteps = fftHeight / (ImGui::CalcTextSize("000.000").y);

        range = findBestRange(viewBandwidth, maxHSteps);
        vRange = findBestRange(fftMax - fftMin, maxVSteps);

        updateWaterfallFb();
        updateAllVFOs();
    }

    bool WaterFall::menuDialog()
    {
        bool open = true;
        // flog::warn("iffinded_freq {0}!", iffinded_freq);
        gui::mainWindow.lockWaterfallControls = true;
        std::string id = "Delete##del_item_wf";
        ImGui::OpenPopup(id.c_str());
        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::Text("Видалити відмічену частоту?");
            if (ImGui::Button("   OK   "))
            {
                if (iffinded_freq)
                {
                    // Блокируем мьютекс НА ВСЁ ВРЕМЯ операции
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);

                    for (auto it = finded_freq.begin(); it != finded_freq.end(); /* no increment */)
                    {
                        if (it->second.selected)
                        {
                            // Безопасно удаляем и получаем итератор на следующий элемент
                            it = finded_freq.erase(it);
                        }
                        else
                        {
                            // Просто переходим к следующему
                            ++it;
                        }
                    }
                }
                else
                {
                    // То же самое для skip_finded_freq
                    std::lock_guard<std::mutex> lock(gui::waterfall.skipFreqMutex);

                    for (auto it = skip_finded_freq.begin(); it != skip_finded_freq.end(); /* no increment */)
                    {
                        if (it->second.selected)
                        {
                            it = skip_finded_freq.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
                open = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати"))
            {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    void WaterFall::draw()
    {
        buf_mtx.lock();
        window = GetCurrentWindow();

        widgetPos = ImGui::GetWindowContentRegionMin();
        widgetEndPos = ImGui::GetWindowContentRegionMax();
        widgetPos.x += window->Pos.x;
        widgetPos.y += window->Pos.y;
        widgetEndPos.x += window->Pos.x - 4; // Padding
        widgetEndPos.y += window->Pos.y;
        widgetSize = ImVec2(widgetEndPos.x - widgetPos.x, widgetEndPos.y - widgetPos.y);

        if (selectedVFO == "" && vfos.size() > 0)
        {
            selectFirstVFO();
        }

        if (widgetPos.x != lastWidgetPos.x || widgetPos.y != lastWidgetPos.y)
        {
            lastWidgetPos = widgetPos;
            onPositionChange();
        }
        if (widgetSize.x != lastWidgetSize.x || widgetSize.y != lastWidgetSize.y)
        {
            lastWidgetSize = widgetSize;
            onResize();
        }

        // window->DrawList->AddRectFilled(widgetPos, widgetEndPos, IM_COL32( 0, 0, 0, 255 ));
        ImU32 bg = ImGui::ColorConvertFloat4ToU32(gui::themeManager.waterfallBg);
        window->DrawList->AddRectFilled(widgetPos, widgetEndPos, bg);
        window->DrawList->AddRect(widgetPos, widgetEndPos, IM_COL32(50, 50, 50, 255), 0.0, 0, style::uiScale);
        window->DrawList->AddLine(ImVec2(widgetPos.x, freqAreaMax.y), ImVec2(widgetPos.x + widgetSize.x, freqAreaMax.y), IM_COL32(50, 50, 50, 255), style::uiScale);
        if (gui::mainWindow.getStopMenuUI() == false)
        {

            if (!gui::mainWindow.lockWaterfallControls)
            {
                inputHandled = false;
                InputHandlerArgs args;
                args.fftRectMin = fftAreaMin;
                args.fftRectMax = fftAreaMax;
                args.freqScaleRectMin = freqAreaMin;
                args.freqScaleRectMax = freqAreaMax;
                args.waterfallRectMin = wfMin;
                args.waterfallRectMax = wfMax;
                args.lowFreq = lowerFreq;
                args.highFreq = upperFreq;
                args.freqToPixelRatio = (double)dataWidth / viewBandwidth;
                args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
                onInputProcess.emit(args);
                if (!inputHandled)
                {
                    processInputs();
                }
            }
            updateAllVFOs(true);
            if (fftVisible)
            {
                drawFFT();
            }
            if (waterfallVisible)
            {
                drawWaterfall();
            }

            drawVFOs();
            if (bandplan != NULL && bandplanVisible)
            {
                drawBandPlan();
            }
        }
        int shift = 0;
        if (radioMode == 0 || radioMode == 1)
        {
            shift = 60; // 100; //50;
        }
        // flog::info("radioMode={0}, shift {1}", radioMode, shift);
        if (waterfallVisible)
        {
            ImGui::SetCursorPosY(fftHeight + waterfallHeight + shift);
        }
        else
        {
            ImGui::SetCursorPosY(fftHeight + shift);
        }
        //=============== SCANNER2 =========================
        if (radioMode == 0 || radioMode == 1)
        {
            std::string currSource = sourcemenu::getCurrSource();
            // flog::info("currSource {0}", currSource);
            ImGui::BeginTable("draw", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if ((radioMode == 0 || radioMode == 1) && currSource != "APM")
            {
                bool _run = scan2_running;
                float menuWidth = ImGui::GetContentRegionAvail().x;
                int ColWidth = floor(menuWidth / 3);
                if (_run == true)
                {
                    ImGui::BeginDisabled();
                }
                ImGui::BeginTable("draw_search", 3, ImGuiTableFlags_Borders); //   | ImGuiTableFlags_ScrollY
                ImGui::TableNextRow();
                //-----------------------------------------
                ImGui::TableSetColumnIndex(0);

                ImGui::TextColored(ImVec4(1, 0, 1, 1), "        Результати пошуку:     ");
                ImGui::BeginTable("wfscannerREZWT_rez", 4);
                ImGui::TableNextRow();
                //-----------------------------
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Button("Завантажити##wf_scan_rez_get_2", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    try
                    {
                        rez_importOpen1 = true;
                        core::configManager.acquire();
                        std::string pathValid = core::configManager.getPath() + "/Banks/*";

                        try
                        {
                            std::string jsonPath = core::configManager.conf["PathJson"];
                            fs::create_directory(jsonPath);
                            pathValid = jsonPath + "/Search";
                            fs::create_directory(pathValid);
                            pathValid = pathValid + "/*";
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << e.what() << '\n';
                            pathValid = core::configManager.getPath() + "/Banks/*";
                        }
                        core::configManager.release();
                        importDialog = new pfd::open_file("Import bookmarks", pathValid, {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                    }
                    catch (const std::exception &e)
                    {
                        // std::cerr << e.what() << '\n';
                    }
                }
                if (rez_importOpen1 && importDialog->ready())
                {
                    rez_importOpen1 = false;

                    std::vector<std::string> paths = importDialog->result();

                    // if (paths.size() > 0 && listNames.size() > 0) { //
                    if (paths.size() > 0)
                    { //

                        try
                        {
                            importBookmarks(paths[0], false);
                        }
                        catch (const std::exception &e)
                        {
                            flog::error("{0}", e.what());
                            this->txt_error = e.what();
                            this->_error = true;
                        }
                    }
                    delete importDialog;
                }
                if (ImGui::GenericDialog("wf_manager_confirm2", _error, GENERIC_DIALOG_BUTTONS_OK, [this]()
                                         { ImGui::TextColored(ImVec4(1, 0, 0, 1), "Помилка імпорту json!  %s", this->txt_error.c_str()); }) == GENERIC_DIALOG_BUTTON_OK)
                {
                    rez_importOpen1 = false;
                    _error = false;
                };

                //--------------------------------
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Button("На сканування...##wf_scan_rez_save_2", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    try
                    {
                        core::configManager.acquire();
                        std::string pathValid = core::configManager.getPath() + "/Banks/*";
                        int InstNum = 0;
                        try
                        {
                            std::string jsonPath = core::configManager.conf["PathJson"];
                            flog::info("jsonPath '{0}'", jsonPath);
                            InstNum = core::configManager.conf["InstanceNum"];
                            fs::create_directory(jsonPath);
                            pathValid = jsonPath + "/Scan";
                            flog::info("pathValid '{0}'", pathValid);
                            fs::create_directory(pathValid);
                            // pathValid = pathValid + "/*";
                            flog::info("pathValid '{0}'", pathValid);
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << e.what() << '\n';
                            pathValid = core::configManager.getPath() + "/Banks";
                        }
                        core::configManager.release();

                        rez_exportedBookmarks = json::object();
                        bool _frst = true;
                        std::string firsfreq = "2";
                        rez_exportedBookmarks["domain"] = "freqs-bank";
                        rez_exportedBookmarks["rx-mode"] = "scanning";
                        rez_exportedBookmarks["bank-name"] = selectedListName;

                        time_t rawtime;
                        struct tm *timeinfo;
                        char buffer[80];
                        time(&rawtime);
                        timeinfo = localtime(&rawtime);
                        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
                        std::string s(buffer);

                        rez_exportedBookmarks["time_created"] = s;
                        rez_exportedBookmarks["InstNum"] = InstNum;
                        int i = 0;
                        std::string bmName = "";
                        {
                            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                            for (const auto &[key, bm] : finded_freq)
                            {
                                i++;
                                unsigned int _frec = round(bm.frequency);
                                if (_frst == true)
                                {
                                    // std::string bmfreq = std::to_string(_frec) + ".0";
                                    firsfreq = std::to_string(_frec);
                                    _frst = false;
                                }
                                bmName = std::to_string(i);
                                rez_exportedBookmarks[selectedListName]["bookmarks"][bmName]["frequency"] = bm.frequency;
                                rez_exportedBookmarks[selectedListName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
                                rez_exportedBookmarks[selectedListName]["bookmarks"][bmName]["level"] = bm.level;
                                rez_exportedBookmarks[selectedListName]["bookmarks"][bmName]["mode"] = bm.mode;
                                // flog::info("bm.frequency'{0}'", bm.frequency);
                            }
                        }
                        rez_exportOpen = true;
                        flog::info("pathValid '{0}'", pathValid);
                        std::string expname = pathValid + "/scan_2_" + selectedListName + "_" + firsfreq + ".json";
                        exportDialog = new pfd::save_file("Export results", expname.c_str(), {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << e.what() << '\n';
                    }
                }

                //-----------------------------------
                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("Зберегти##wf_freq_rez_save_2", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    try
                    {
                        core::configManager.acquire();
                        std::string pathValid = core::configManager.getPath() + "/Banks/*";
                        int InstNum = 0;
                        try
                        {
                            std::string jsonPath = core::configManager.conf["PathJson"];
                            flog::info("jsonPath '{0}'", jsonPath);
                            InstNum = core::configManager.conf["InstanceNum"];
                            fs::create_directory(jsonPath);
                            pathValid = jsonPath + "/Search";
                            flog::info("pathValid '{0}'", pathValid);
                            fs::create_directory(pathValid);
                            // pathValid = pathValid + "/*";
                            flog::info("pathValid '{0}'", pathValid);
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << e.what() << '\n';
                            pathValid = core::configManager.getPath() + "/Banks";
                        }
                        core::configManager.release();

                        rez_exportedBookmarks = json::object();
                        bool _frst = true;
                        std::string firsfreq = "2";
                        rez_exportedBookmarks["domain"] = "freqs-bank";
                        rez_exportedBookmarks["rx-mode"] = "search";
                        rez_exportedBookmarks["bank-name"] = selectedListName;

                        time_t rawtime;
                        struct tm *timeinfo;
                        char buffer[80];
                        time(&rawtime);
                        timeinfo = localtime(&rawtime);
                        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
                        std::string s(buffer);

                        rez_exportedBookmarks["time_created"] = s;
                        rez_exportedBookmarks["InstNum"] = InstNum;
                        std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);

                        for (const auto &[key, bm] : finded_freq)
                        {
                            unsigned int _frec = round(bm.frequency);
                            std::string bmfreq = std::to_string(_frec) + ".0";
                            if (_frst == true)
                            {
                                firsfreq = std::to_string(_frec);
                                _frst = false;
                            }
                            rez_exportedBookmarks["scan"][bmfreq]["frequency"] = bm.frequency;
                            rez_exportedBookmarks["scan"][bmfreq]["bandwidth"] = bm.bandwidth;
                            rez_exportedBookmarks["scan"][bmfreq]["level"] = bm.level;
                            rez_exportedBookmarks["scan"][bmfreq]["mode"] = bm.mode;
                            rez_exportedBookmarks["scan"][bmfreq]["ftime"] = bm.ftime;
                        }
                        rez_exportOpen = true;
                        flog::info("pathValid '{0}'", pathValid);
                        std::string expname = pathValid + "/search_" + selectedListName + "_" + firsfreq + ".json";
                        exportDialog = new pfd::save_file("Export results", expname.c_str(), {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << e.what() << '\n';
                    }
                }

                //---------------------------------------
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button("Очистити##wf_freq_rez_clear_2", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    finded_freq.clear();
                }
                // rez_importOpen = false;
                //___________________________________________________________
                ImGui::EndTable();

                if (rez_exportOpen && exportDialog->ready())
                {
                    rez_exportOpen = false;
                    std::string path = exportDialog->result();
                    flog::info("path '{0}'", path);

                    if (path != "")
                    {
                        exportBookmarks(path);
                    }
                    delete exportDialog;
                }
                // ImGui::TableNextRow();
                std::vector<double> selectedNames;
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    for (auto &[name, bm] : finded_freq)
                    {
                        if (bm.selected)
                        {
                            selectedNames.push_back(name);
                        }
                    }
                }

                if (ImGui::BeginTable("scanner2_rez_draw", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 210)))
                { //  , ImVec2(0, 210) | ImGuiTableFlags_ScrollY
                    ImGui::TableSetupColumn("Част., МГц");
                    ImGui::TableSetupColumn("Вид демод.");
                    ImGui::TableSetupColumn("Смуга, кГц");
                    ImGui::TableSetupColumn("Сигнал");
                    ImGui::TableSetupColumn("Рівень, дБ");
                    ImGui::TableSetupColumn("Час");
                    ImGui::TableSetupScrollFreeze(2, 1);
                    ImGui::TableHeadersRow();

                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    for (auto &[key, fbm] : finded_freq)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        double t_frequency = 0;
                        if (ImGui::Selectable((utils::formatFreqMHz(key) + "##wfsrch3_rez_name").c_str(), &fbm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick))
                        {
                            // for (auto &[_name, _bm] : gui::waterfall.finded_freq_copy)
                            if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl)
                            {
                                // std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                                for (auto &[_name, _bm] : finded_freq)
                                {
                                    if (key == _name)
                                    {
                                        _bm.selected = true;
                                        t_frequency = _bm.frequency;
                                        continue;
                                    }
                                    _bm.selected = false;
                                }
                            }
                        }
                        /*
                        if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            bool t_work = gui::mainWindow.getIfOneButtonStart();
                            flog::info("IsMouseDoubleClicked t_frequency {0}, t_work {1}", t_frequency, t_work);
                            if (t_frequency > 10000 && !t_work)
                                applyBookmark(t_frequency, gui::waterfall.selectedVFO);
                        }
                        */
                        if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered())
                        {
                            bool t_work = gui::mainWindow.getIfOneButtonStart();

                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            {
                                bool t_work = gui::mainWindow.getIfOneButtonStart();
                                flog::info("IsMouseDoubleClicked t_frequency {0}, t_work {1}", t_frequency, t_work);
                                if (!t_work)
                                    applyBookmark(fbm, gui::waterfall.selectedVFO);
                            }
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            {
                                menuListOpen = true;
                                iffinded_freq = true;
                            }
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", demodModeList[fbm.mode]);

                        ImGui::TableSetColumnIndex(2);
                        int bw = round(fbm.bandwidth / 1000);
                        std::string sbw = std::to_string(bw);
                        if (bw == 13)
                            sbw = "12.5";
                        if (bw == 6)
                            sbw = "6.25";

                        ImGui::Text("%s", sbw.c_str());

                        ImGui::TableSetColumnIndex(3);
                        /*
                        std::string signal = "";
                        if (SignalIndf) {
                            if (fbm.Signal == 1)
                                signal = "ТЛФ"; // "Noise";
                            else if (fbm.Signal == 2)
                                signal = "DMR"; // "Noise";
                            else
                                signal = "НВ"; // "Noise";
                        }
                        */
                        std::string signal = "";
                        if (fbm.Signal == 1)
                            signal = "Мова"; // "Noise";
                        else if (fbm.Signal == 2)
                            signal = "DMR"; // "Noise";
                        else
                            signal = "НВ";
                        ImGui::Text("%s", signal.c_str());

                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%s", std::to_string(fbm.level).c_str());

                        ImGui::TableSetColumnIndex(5);
                        char timeString[std::size("hh:mm:ss")];
                        std::strftime(std::data(timeString), std::size(timeString), "%T", std::localtime(&fbm.ftime)); // std::gmtime(&fbm.ftime));

                        // std::strftime(std::data(timeString), std::size(timeString), "%FT%T", std::gmtime(&fbm.ftime));
                        ImGui::Text("%s", timeString);

                        ImVec2 max = ImGui::GetCursorPos();
                    }

                    ImGui::EndTable();
                }
                if (_run == true)
                {
                    ImGui::EndDisabled();
                }
                //-----------------------------------------
                ImGui::TableSetColumnIndex(1);
                // menuWidth = ImGui::GetContentRegionAvail().x;
                float _width = ColWidth / 3;
                {
                    ImGui::BeginTable("draw_button", 3);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text(" ");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text(" ");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text(" ");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableSetColumnIndex(1);
                    if (selectedNames.size() == 0)
                    {
                        style::beginDisabled();
                    }

                    if (ImGui::Button("     > ##remove_to_blacklist", ImVec2(_width, 0)))
                    {
                        // std::scoped_lock lock(gui::waterfall.findedFreqMtx, gui::waterfall.skipFreqMutex);
                        // 2. Теперь выполняем весь цикл под защитой.
                        for (double dfrec : selectedNames)
                        {
                            // std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                            auto it = finded_freq.find(dfrec);
                            if (it != finded_freq.end())
                            {
                                const auto &source_bookmark = it->second;
                                auto &dest_bookmark = skip_finded_freq[dfrec];
                                dest_bookmark.frequency = source_bookmark.frequency;
                                dest_bookmark.selected = source_bookmark.selected;
                                dest_bookmark.level = source_bookmark.level;
                                dest_bookmark.mode = source_bookmark.mode;
                                dest_bookmark.bandwidth = source_bookmark.bandwidth;
                                dest_bookmark.ftime = time(0);
                            }
                        }
                    }
                    if (selectedNames.size() == 0)
                    {
                        style::endDisabled();
                    }

                    ImGui::TableSetColumnIndex(2);

                    ImGui::EndTable();
                }
                //-----------------------------------------
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ImVec4(1, 0, 1, 1), "        Не становлять інтересу :");
                ImGui::BeginTable("scan_skip2_buttons", 4);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // rez_importOpen = false;
                if (ImGui::Button("Додати##scan2_get_curr_freq", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    double current = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                    // FoundBookmark _addFreq;
                    SkipFoundBookmark _addSkipFreq;
                    int _mode = 0;
                    int RADIO_IFACE_CMD_GET_MODE = 0;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                    double curr_bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                    time_t now = time(0);

                    _addSkipFreq.frequency = current;
                    _addSkipFreq.level = 100;
                    _addSkipFreq.mode = _mode;
                    _addSkipFreq.bandwidth = curr_bandwidth;
                    _addSkipFreq.selected = false;
                    _addSkipFreq.ftime = now;

                    skip_finded_freq[current] = _addSkipFreq;
                    flog::info("_addSkipFreq.frequency {0}", _addSkipFreq.frequency);
                }
                ImGui::TableSetColumnIndex(1);

                if (ImGui::Button("Завантажити##scan2_get", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    core::configManager.acquire();
                    rez_importOpen2 = true;
                    std::string pathValid = core::configManager.getPath() + "/Banks/*";
                    try
                    {
                        std::string jsonPath = core::configManager.conf["PathJson"];
                        fs::create_directory(jsonPath);
                        pathValid = jsonPath + "/Search";
                        fs::create_directory(pathValid);
                        pathValid = pathValid + "/*";
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << e.what() << '\n';
                        pathValid = core::configManager.getPath() + "/Banks/*";
                    }
                    core::configManager.release();
                    try
                    {

                        importDialog = new pfd::open_file("Import bookmarks", pathValid, {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                    }
                    catch (const std::exception &e)
                    {
                    }
                }
                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("Зберегти##wf_freq_rez_save_skip2", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    try
                    {
                        core::configManager.acquire();
                        std::string pathValid = core::configManager.getPath() + "/Banks/*";
                        int InstNum = 0;
                        try
                        {
                            std::string jsonPath = core::configManager.conf["PathJson"];
                            flog::info("jsonPath '{0}'", jsonPath);
                            InstNum = core::configManager.conf["InstanceNum"];
                            fs::create_directory(jsonPath);
                            pathValid = jsonPath + "/Search";
                            flog::info("pathValid '{0}'", pathValid);
                            fs::create_directory(pathValid);
                            // pathValid = pathValid + "/*";
                            flog::info("pathValid '{0}'", pathValid);
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << e.what() << '\n';
                            pathValid = core::configManager.getPath() + "/Banks";
                        }
                        core::configManager.release();

                        rez_exportedBookmarks = json::object();
                        bool _frst = true;
                        std::string firsfreq = "2";
                        rez_exportedBookmarks["domain"] = "freqs-bank";
                        rez_exportedBookmarks["rx-mode"] = "search";
                        rez_exportedBookmarks["bank-name"] = selectedListName;

                        time_t rawtime;
                        struct tm *timeinfo;
                        char buffer[80];
                        time(&rawtime);
                        timeinfo = localtime(&rawtime);
                        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
                        std::string s(buffer);

                        rez_exportedBookmarks["time_created"] = s;
                        rez_exportedBookmarks["InstNum"] = InstNum;
                        for (const auto &[key, fbm] : skip_finded_freq)
                        {
                            unsigned int _frec = round(key);
                            flog::info("bm.frequency'{0}', _frec {1}", fbm.frequency, _frec);
                            std::string bmfreq = std::to_string(_frec) + ".0";
                            if (_frst == true)
                            {
                                firsfreq = std::to_string(_frec);
                                _frst = false;
                            }
                            rez_exportedBookmarks["scan"][bmfreq]["frequency"] = fbm.frequency;
                            rez_exportedBookmarks["scan"][bmfreq]["bandwidth"] = fbm.bandwidth;
                            rez_exportedBookmarks["scan"][bmfreq]["level"] = fbm.level;
                            rez_exportedBookmarks["scan"][bmfreq]["mode"] = fbm.mode;
                            rez_exportedBookmarks["scan"][bmfreq]["ftime"] = fbm.ftime;
                            flog::info("bm.frequency'{0}'", fbm.frequency);
                        }
                        std::string expname = pathValid + "/black_list.json";
                        flog::info("expname '{0}'", expname);
                        exportBookmarks(expname);
                        // rez_exportOpen = true;
                        // std::string expname = pathValid + "/search_" + selectedListName + "_" + firsfreq + ".json";
                        // exportDialog = new pfd::save_file("Export results", expname.c_str(), { "JSON Files (*.json)", "*.json", "All Files", "*" }, pfd::opt::multiselect);
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << e.what() << '\n';
                    }
                }
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button("Очистити##scan_skip2_clear", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.skipFreqMutex);
                    skip_finded_freq.clear();
                }
                /*
                if (rez_exportOpen && exportDialog->ready()) {
                    rez_exportOpen = false;
                    std::string path = exportDialog->result();
                    flog::info("path '{0}'", path);

                    if (path != "") {
                        exportBookmarks(path);
                    }
                    delete exportDialog;
                }
                */

                if (rez_importOpen2 && importDialog->ready())
                {
                    rez_importOpen2 = false;
                    std::vector<std::string> paths = importDialog->result();
                    if (paths.size() > 0)
                    {
                        try
                        {
                            importBookmarks(paths[0], true);
                        }
                        catch (const std::exception &e)
                        {
                            flog::error("{0}", e.what());
                            txt_error = e.what();
                            _error = true;
                        }
                    }
                    delete importDialog;
                }
                if (ImGui::GenericDialog("skip_finded_freq_confirm", _error, GENERIC_DIALOG_BUTTONS_OK, [this]()
                                         { ImGui::TextColored(ImVec4(1, 0, 0, 1), "Помилка імпорту json!  %s", txt_error.c_str()); }) == GENERIC_DIALOG_BUTTON_OK)
                {
                    rez_importOpen2 = false;
                    _error = false;
                };
                ImGui::EndTable();
                std::vector<double> selectedSkipNames;
                for (auto &[name, bm] : skip_finded_freq)
                {
                    if (bm.selected)
                    {
                        selectedSkipNames.push_back(name);
                    }
                }
                if (ImGui::BeginTable("skip2_rez_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 210)))
                { //  | ImGuiTableFlags_ScrollY, 210
                    ImGui::TableSetupColumn("Част., МГц");
                    ImGui::TableSetupColumn("Вид демод.");
                    ImGui::TableSetupColumn("Смуга, кГц");
                    ImGui::TableSetupScrollFreeze(2, 1);
                    ImGui::TableHeadersRow();

                    if (skip_finded_freq.size() > 0)
                    {
                        for (const auto &[key, fbm] : skip_finded_freq)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            // ImGui::Text("   ");
                            ImVec2 min = ImGui::GetCursorPos();
                            if (ImGui::Selectable((utils::formatFreqMHz(key) + "##wfskip_rez_name").c_str(), &fbm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick))
                            {
                                if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl)
                                {
                                    for (auto &[_name, _bm] : skip_finded_freq)
                                    {
                                        if (key == _name)
                                        {
                                            continue;
                                        }
                                        _bm.selected = false;
                                        flog::info("{0}, _name {1}", key, _name);
                                    }
                                }
                            }

                            if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered())
                            {
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                {
                                    applySkipBookmark(fbm, gui::waterfall.selectedVFO);
                                }
                                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                                {
                                    menuListOpen = true;
                                    iffinded_freq = false;
                                }
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", demodModeList[fbm.mode]);
                            ImGui::TableSetColumnIndex(2);
                            int bw = round(fbm.bandwidth / 1000);
                            std::string sbw = std::to_string(bw);
                            if (bw == 13)
                                sbw = "12.5";
                            if (bw == 6)
                                sbw = "6.25";
                            ImGui::Text("%s", sbw.c_str());
                            // ImVec2 max = ImGui::GetCursorPos();
                        }
                    }
                    else
                    {
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("   ");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("   ");
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("   ");
                    }
                }
                ImGui::EndTable();

                // ImGui::TableSetColumnIndex(3);
                ImGui::EndTable();
            }
            //-------------------------------------------------------
            else if (currSource == SOURCE_ARM)
            {
                //---------------------------------------------------
                ImVec2 btnSizePng(20, 20);
                ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

                ImGui::BeginTable("rezult_draw", SERVERS_Count, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
                ImGui::TableNextRow();

                float menuWidth = ImGui::GetContentRegionAvail().x;
                int ctrl = -1;
                for (int i = 0; i < 8; i++)
                {
                    if (gui::mainWindow.getServerStatus(i) == ARM_STATUS_FULL_CONTROL)
                    {
                        ctrl = i;
                    }
                }
                if (ctrl == -1)
                {
                    for (int i = 0; i < 8; i++)
                    {
                        if (gui::mainWindow.getServerStatus(i) == ARM_STATUS_STAT_CONTROL)
                        {
                            gui::mainWindow.setServerStatus(i, ARM_STATUS_FULL_CONTROL);
                            gui::mainWindow.setCurrServer(i);
                            gui::mainWindow.setPlayState(false);
                            gui::mainWindow.setUpdateServerStatus(i, true);
                            break;
                        }
                    }
                }
                //======================================================================
                float _width = 0;
                for (uint8_t CHNL = 0; CHNL < SERVERS_Count; CHNL++)
                { // MAX_SERVERS
                    // int CHNL = 0;
                    ImGui::TableSetColumnIndex(CHNL);
                    if (CHNL == 0)
                    {
                        menuWidth = ImGui::GetContentRegionAvail().x;
                        _width = menuWidth - ImGui::GetCursorPosX() + 10;
                    }
                    int stats = gui::mainWindow.getServerStatus(CHNL);
                    bool button_srch = gui::mainWindow.getbutton_srch(CHNL);
                    bool button_scan = gui::mainWindow.getbutton_scan(CHNL);
                    bool button_ctrl = gui::mainWindow.getbutton_ctrl(CHNL);
                    bool recording = gui::mainWindow.getServerRecording(CHNL);
                    searchListId[CHNL] = gui::mainWindow.getidOfList_srch(CHNL);
                    // selectedMode[CHNL] = gui::mainWindow.getS .getselectedLogicId(CHNL);
                    selectedMode[CHNL] = gui::mainWindow.getSelectedMode(CHNL);
                    scanListId[CHNL] = gui::mainWindow.getidOfList_scan(CHNL);
                    ctrlListId[CHNL] = gui::mainWindow.getidOfList_ctrl(CHNL);

                    bool Runnung = false;
                    if (gui::mainWindow.getButtonStart(CHNL))
                        Runnung = true;
                    // if (button_srch || button_scan || button_ctrl || recording) Runnung = true;
                    // flog::info("{0}. stat {1} [button_srch {2}, button_scan {3}, button_ctrl{4}, recording {5}]", CHNL, selectedMode[CHNL], button_srch, button_scan, button_ctrl, recording );
                    if (button_srch)
                    {
                        selectedMode[CHNL] = 1;
                        gui::mainWindow.setSelectedMode(CHNL, 1);
                    }
                    else if (button_scan)
                    {
                        selectedMode[CHNL] = 2;
                        gui::mainWindow.setSelectedMode(CHNL, 2);
                    }
                    else if (button_ctrl)
                    {
                        selectedMode[CHNL] = 3;
                        gui::mainWindow.setSelectedMode(CHNL, 3);
                    }
                    else if (recording)
                    {
                        selectedMode[CHNL] = 0;
                        gui::mainWindow.setSelectedMode(CHNL, 0);
                    }

                    // ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                    if (stats == ARM_STATUS_NOT_CONTROL)
                    { // && gui::mainWindow.getServerStatus(CHNL)!=0
                        if (gui::mainWindow.getServerStatus(CHNL) != ARM_STATUS_NOT_CONTROL)
                        {
                            gui::mainWindow.setServerStatus(CHNL, ARM_STATUS_NOT_CONTROL);
                            flog::info("setServerStatus 1");
                        }
                        if (ImGui::ImageButton(icons::NET_ERROR, btnSizePng, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false))
                        {
                            // gui::mainWindow.setUpdateServerStatus(0, true);
                        }
                    }
                    else
                    {
                        if (ImGui::ImageButton(icons::NET_OK, btnSizePng, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false))
                        {
                            // gui::mainWindow.setUpdateServerStatus(0, true);
                        }
                    }
                    std::string SearchListTxt = "General";
                    std::string ScanListTxt = "General";
                    std::string CTRLListTxt = "General";

                    if (gui::mainWindow.getUpdateSrchListForBotton(CHNL))
                    {
                        SearchListTxt = gui::mainWindow.getSearchListNamesTxt(CHNL);
                        flog::info("TRACE. Update SearchListTxt, CHNL {0} getUpdateSrchListForBotton SearchListTxt {1}", CHNL, SearchListTxt);
                        gui::mainWindow.setUpdateSrchListForBotton(CHNL, false);
                    }
                    else
                        SearchListTxt = gui::mainWindow.getSearchListNamesTxt(CHNL);

                    if (gui::mainWindow.getUpdateScanListForBotton(CHNL))
                    {
                        ScanListTxt = gui::mainWindow.getScanListNamesTxt(CHNL);
                        flog::info("TRACE. Update ScanListTxt {0} getUpdateSrchListForBotton ScanListTxt {1}", CHNL, ScanListTxt);
                        gui::mainWindow.setUpdateScanListForBotton(CHNL, false);
                    }
                    else
                        ScanListTxt = gui::mainWindow.getScanListNamesTxt(CHNL);

                    if (gui::mainWindow.getUpdateCTRLListForBotton(CHNL))
                    {
                        CTRLListTxt = gui::mainWindow.getCTRLListNamesTxt(CHNL);
                        flog::info("TRACE. Update CTRLListTxt {0} getUpdateCTRLListForBotton CTRLListTxt {1}", CHNL, CTRLListTxt);
                        gui::mainWindow.setUpdateCTRLListForBotton(CHNL, false);
                    }
                    else
                        CTRLListTxt = gui::mainWindow.getCTRLListNamesTxt(CHNL);

                    // flog::info("stats = {0}", stats);
                    ImGui::SameLine();
                    // if (stats == ARM_STATUS_NOT_CONTROL) ImGui::BeginDisabled();
                    if (stats == ARM_STATUS_NOT_CONTROL || stats == ARM_STATUS_STAT_CONTROL)
                    {
                        ImTextureID ITicon = icons::NET_STATUS1;
                        switch (CHNL)
                        {
                        case 0:
                            ITicon = icons::NET_STATUS1;
                            break;
                        case 1:
                            ITicon = icons::NET_STATUS2;
                            break;
                        case 2:
                            ITicon = icons::NET_STATUS3;
                            break;
                        case 3:
                            ITicon = icons::NET_STATUS4;
                            break;
                        case 4:
                            ITicon = icons::NET_STATUS5;
                            break;
                        case 5:
                            ITicon = icons::NET_STATUS6;
                            break;
                        case 6:
                            ITicon = icons::NET_STATUS7;
                            break;
                        case 7:
                            ITicon = icons::NET_STATUS8;
                            break;
                        }
                        if (stats == ARM_STATUS_NOT_CONTROL)
                        {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::ImageButton(ITicon, btnSizePng, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false))
                        {
                            flog::info(" !!!!     {0}  stats {1} == ARM_STATUS_NOT_CONTROL || stats == ARM_STATUS_STAT_CONTROL 1 ", CHNL, stats);
                            if (gui::mainWindow.getServerStatus(CHNL) == ARM_STATUS_STAT_CONTROL)
                            {
                                gui::mainWindow.setServerStatus(CHNL, ARM_STATUS_FULL_CONTROL);
                                gui::mainWindow.setPlayState(false);
                                gui::mainWindow.setUpdateServerStatus(CHNL, true);
                                gui::mainWindow.setCurrServer(CHNL);
                            }
                            if (displaymenu::getFFTHold())
                            {
                                displaymenu::setFFTHold(false);
                                gui::waterfall.setFFTHold(false);
                                /*for (int i = 0; i < dataWidth; i++) {
                                    latestFFT[i] = -1000.0f; // Hide everything
                                    latestFFTHold[i] = -1000.0f;
                                } */
                            }
                        }
                        if (stats == ARM_STATUS_NOT_CONTROL)
                        {
                            ImGui::EndDisabled();
                        }
                    }
                    else if (stats == ARM_STATUS_FULL_CONTROL)
                    {
                        ImTextureID ITicon = icons::NET_STATUS1;
                        switch (CHNL)
                        {
                        case 0:
                            ITicon = icons::NET_HUB1;
                            break;
                        case 1:
                            ITicon = icons::NET_HUB2;
                            break;
                        case 2:
                            ITicon = icons::NET_HUB3;
                            break;
                        case 3:
                            ITicon = icons::NET_HUB4;
                            break;
                        case 4:
                            ITicon = icons::NET_HUB5;
                            break;
                        case 5:
                            ITicon = icons::NET_HUB6;
                            break;
                        case 6:
                            ITicon = icons::NET_HUB7;
                            break;
                        case 7:
                            ITicon = icons::NET_HUB8;
                            break;
                        }

                        if (ImGui::ImageButton(ITicon, btnSizePng, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false))
                        {
                            /*
                                flog::info("      {0}  stats {1} == ARM_STATUS_NOT_CONTROL || stats == ARM_STATUS_STAT_CONTROL 2 ", CHNL, stats);
                                gui::mainWindow.setServerStatus(CHNL, ARM_STATUS_STAT_CONTROL);
                                gui::mainWindow.setPlayState(false);
                                gui::mainWindow.setUpdateServerStatus(CHNL, true);
                            */
                        }
                    }
                    // if (stats == ARM_STATUS_NOT_CONTROL) ImGui::EndDisabled();
                    ImGui::SameLine();
                    std::string nameSrv = gui::mainWindow.getServersName(CHNL);
                    if (nameSrv == "")
                        nameSrv = "П" + std::to_string(CHNL + 1);
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", nameSrv.c_str());

                    double frec = gui::mainWindow.getServersFreq(CHNL);
                    // int ifreq = round(frec);
                    std::string str = utils::formatFreqMHz(round(frec)) + " МГц";
                    ImGui::TextColored(ImVec4(1, 0, 1, 1), "%s", str.c_str());
                    // stats = gui::mainWindow.getServerStatus(CHNL);
                    if (stats == ARM_STATUS_NOT_CONTROL)
                        ImGui::BeginDisabled();
                    if (Runnung)
                        ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(_width);

                    if (ImGui::Combo(("##arm1_mode" + std::to_string(CHNL)).c_str(), &selectedMode[CHNL], ModeTxt.c_str()))
                    {
                        flog::info("       selectedMode[{0}] {1} ", CHNL, selectedMode[CHNL]);
                        core::configManager.conf["receivers"][CHNL]["mode"] = selectedMode[CHNL];
                        gui::mainWindow.setSelectedMode(CHNL, selectedMode[CHNL]);
                    }
                    if (selectedMode[CHNL] == 1)
                    {
                        ImGui::SetNextItemWidth(_width);
                        if (ImGui::Combo(("##arm1_search" + std::to_string(CHNL)).c_str(), &searchListId[CHNL], SearchListTxt.c_str()))
                        {
                            flog::info("    searchListId[{0}] = {1}", CHNL, searchListId[CHNL]);
                            gui::mainWindow.setidOfList_srch(CHNL, searchListId[CHNL]);
                        }
                    }
                    else if (selectedMode[CHNL] == 2)
                    {
                        ImGui::SetNextItemWidth(_width);
                        if (ImGui::Combo(("##arm1_scan" + std::to_string(CHNL)).c_str(), &scanListId[CHNL], ScanListTxt.c_str()))
                        {
                            flog::info("    scanListId[{0}] = {1}", CHNL, scanListId[CHNL]);
                            gui::mainWindow.setidOfList_scan(CHNL, scanListId[CHNL]);
                        }
                    }
                    else if (selectedMode[CHNL] == 3)
                    {
                        ImGui::SetNextItemWidth(_width);
                        if (ImGui::Combo(("##arm1_ctrl" + std::to_string(CHNL)).c_str(), &ctrlListId[CHNL], CTRLListTxt.c_str()))
                        {
                            // flog::info("    ctrlListId[{0}] = {1}", CHNL, ctrlListId[CHNL]);
                            gui::mainWindow.setidOfList_ctrl(CHNL, ctrlListId[CHNL]);
                        }
                    }
                    if (Runnung)
                        ImGui::EndDisabled();

                    if (selectedMode[CHNL] == 0)
                    {
                        bool rec = gui::mainWindow.getServerRecording(CHNL);
                        if (!rec)
                        {
                            if (ImGui::Button(("ЗАПИС##mode_record_start" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                            {
                                gui::mainWindow.setServerRecordingStart(CHNL);
                                gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                core::configManager.acquire();
                                core::configManager.conf["receivers"][CHNL]["mode"] = 0;
                                core::configManager.conf["receivers"][CHNL]["bank"] = 0;
                                core::configManager.release(true);
                            }
                        }
                        else
                        {
                            if (ImGui::Button(("ЗУПИНИТИ##mode_record_stop" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                            {
                                gui::mainWindow.setServerRecordingStop(CHNL);
                                gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                            }
                        }
                    }
                    else
                    {
                        if (selectedMode[CHNL] == 1)
                        {
                            if (button_srch)
                            {
                                if (ImGui::Button(("СТОП##mode_srch_stop" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_srch(CHNL, false);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                }
                            }
                            else
                            {
                                if (ImGui::Button(("СТАРТ##mode_srch_start" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_srch(CHNL, true);
                                    gui::mainWindow.setidOfList_srch(CHNL, searchListId[CHNL]);
                                    gui::mainWindow.setUpdateListRcv5Srch(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd5Srch(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                    flog::info("    SET searchListId[{0}] = {1}, srch {1}", CHNL, searchListId[CHNL], gui::mainWindow.getbutton_srch(CHNL));
                                    core::configManager.acquire();
                                    core::configManager.conf["receivers"][CHNL]["mode"] = 1;
                                    core::configManager.conf["receivers"][CHNL]["bank"] = searchListId[CHNL];
                                    core::configManager.release(true);
                                }
                            }
                        }
                        else if (selectedMode[CHNL] == 2)
                        {
                            if (button_scan)
                            {
                                if (ImGui::Button(("СТОП##mode_scan_stop" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_scan(CHNL, false);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                }
                            }
                            else
                            {
                                if (ImGui::Button(("СТАРТ##mode_scan_start" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_scan(CHNL, true);
                                    gui::mainWindow.setidOfList_scan(CHNL, scanListId[CHNL]);
                                    gui::mainWindow.setUpdateListRcv6Scan(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd6Scan(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                    flog::info("    SET scanListId[{0}] = {1}, scan {1}", CHNL, scanListId[CHNL], gui::mainWindow.getbutton_scan(CHNL));
                                    core::configManager.acquire();
                                    core::configManager.conf["receivers"][CHNL]["mode"] = 2;
                                    core::configManager.conf["receivers"][CHNL]["bank"] = scanListId[CHNL];
                                    core::configManager.release(true);
                                }
                            }
                        }
                        else if (selectedMode[CHNL] == 3)
                        {
                            if (button_ctrl)
                            {
                                if (ImGui::Button(("СТОП##mode_ctrl_stop" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_ctrl(CHNL, false);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                }
                            }
                            else
                            {
                                // bool _empty = ((_this->bookmarks_size == 0) ? true : false);
                                // if (_empty)
                                //    ImGui::BeginDisabled();
                                if (ImGui::Button(("СТАРТ##mode_ctrl_start" + std::to_string(CHNL)).c_str(), ImVec2(menuWidth, 0)))
                                {
                                    gui::mainWindow.setbutton_ctrl(CHNL, true);
                                    gui::mainWindow.setidOfList_ctrl(CHNL, ctrlListId[CHNL]);
                                    gui::mainWindow.setUpdateListRcv7Ctrl(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd7Ctrl(CHNL, true);
                                    gui::mainWindow.setUpdateMenuSnd0Main(CHNL, true);
                                    flog::info("    SET ctrlListId[{0}] = {1}, ctrl {2}", CHNL, ctrlListId[CHNL], gui::mainWindow.getbutton_ctrl(CHNL));
                                    core::configManager.acquire();
                                    core::configManager.conf["receivers"][CHNL]["mode"] = 3;
                                    core::configManager.conf["receivers"][CHNL]["bank"] = ctrlListId[CHNL];
                                    core::configManager.release(true);
                                }
                            }
                        }
                    }
                    if (stats == ARM_STATUS_NOT_CONTROL)
                        ImGui::EndDisabled();
                }
                //---------------------------------------------------
                //------------------------------------------------------------------------------------------------------
                /*
                //---------------------------------------------------
                ImGui::TableSetColumnIndex(6);
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Server 7");
                ImGui::TextColored(ImVec4(1, 1, 1, 1), "Статус: Сканування. Банк частот: General");
                //---------------------------------------------------
                ImGui::TableSetColumnIndex(7);
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Server 8");
                ImGui::TextColored(ImVec4(1, 1, 1, 1), "Статус: Сканування");
                */
                ImGui::EndTable();
            }

            ImGui::EndTable();
        }
        if (!waterfallVisible)
        {
            buf_mtx.unlock();
            return;
        }
        if (menuListOpen)
        {
            menuListOpen = menuDialog();
        }

        buf_mtx.unlock();
    }

#define RADIO_IFACE_CMD_SET_MODE 1
#define RADIO_IFACE_CMD_SET_BANDWIDTH 3

    void WaterFall::applyBookmark(FoundBookmark bm, std::string vfoName)
    {
        flog::info("applyBookmark core::modComManager.getModuleName(vfoName) ={0}, bm.frequency {1}", core::modComManager.getModuleName(vfoName), bm.frequency);
        if (vfoName == "")
        {
            // TODO: Replace with proper tune call
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }
        else
        {
            if (core::modComManager.interfaceExists(vfoName))
            {
                if (core::modComManager.getModuleName(vfoName) == "radio")
                {
                    int mode = bm.mode;
                    double _bandwidth = (double)bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);
                }
            }
            // tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
            tuner::centerTuning(gui::waterfall.selectedVFO, bm.frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
    }

    void WaterFall::applySkipBookmark(SkipFoundBookmark bm, std::string vfoName)
    {
        if (vfoName == "")
        {
            // TODO: Replace with proper tune call
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }
        else
        {
            if (core::modComManager.interfaceExists(vfoName))
            {
                if (core::modComManager.getModuleName(vfoName) == "radio")
                {
                    int mode = bm.mode;
                    double _bandwidth = (double)bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);
                }
            }
            // tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
            tuner::centerTuning(gui::waterfall.selectedVFO, bm.frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
    }

    void WaterFall::exportBookmarks(std::string path)
    {
        std::ofstream fs(path);
        fs << rez_exportedBookmarks;
        // flog::info("rez_exportedBookmarks 3 '{0}'", rez_exportedBookmarks);
        fs.close();
    }

    bool WaterFall::importBookmarks(std::string path, bool skip_banks)
    {
        std::ifstream fs(path);
        if (!fs.is_open())
        {
            flog::error("Не удалось открыть файл: {0}", path);
            return false;
        }

        json importBookmarks;
        try
        {
            fs >> importBookmarks;
        }
        catch (const json::parse_error &e)
        {
            flog::error("Ошибка парсинга JSON: {0}", e.what());
            return false;
        }

        flog::info("importBookmarks skip_banks {0}", skip_banks);

        if (!importBookmarks.is_object())
        {
            flog::error("Файл JSON должен быть объектом");
            return false;
        }

        if (!importBookmarks.contains("scan") || !importBookmarks["scan"].is_object())
        {
            flog::error("Атрибут 'scan' отсутствует или имеет неверный формат");
            return false;
        }

        if (skip_banks)
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.skipFreqMutex);
            skip_finded_freq.clear();
        }
        else
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            finded_freq.clear();
        }
        _count_Bookmark = 0;

        // Load every bookmark
        for (auto const [key, bm] : importBookmarks["scan"].items())
        {
            if (skip_banks)
            {
                SkipFoundBookmark fbm;
                double dfrec = bm["frequency"];
                fbm.frequency = bm["frequency"];
                fbm.bandwidth = bm["bandwidth"];
                fbm.mode = bm["mode"];
                fbm.level = bm["level"];
                fbm.selected = false;

                try
                {
                    fbm.ftime = bm["ftime"];
                }
                catch (const std::exception &e)
                {
                    fbm.ftime = 0;
                    std::cerr << e.what() << '\n';
                }
                skip_finded_freq[dfrec] = fbm;
            }
            else
            {
                FoundBookmark fbm;
                double dfrec = bm["frequency"];
                fbm.frequency = bm["frequency"];
                fbm.bandwidth = bm["bandwidth"];
                fbm.mode = bm["mode"];
                fbm.level = bm["level"];
                fbm.selected = false;
                try
                {
                    fbm.ftime = bm["ftime"];
                }
                catch (const std::exception &e)
                {
                    fbm.ftime = 0;
                    std::cerr << e.what() << '\n';
                }
                std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                finded_freq[dfrec] = fbm;
            }
            _count_Bookmark++;
        }

        fs.close();
        return true;
    }

    float *WaterFall::getFFTBuffer()
    {
        if (rawFFTs == NULL)
        {
            return NULL;
        }
        buf_mtx.lock();
        if (waterfallVisible)
        {
            currentFFTLine--;
            fftLines++;
            currentFFTLine = ((currentFFTLine + waterfallHeight) % waterfallHeight);
            fftLines = std::min<float>(fftLines, waterfallHeight);
            return &rawFFTs[currentFFTLine * rawFFTSize];
        }
        return rawFFTs;
    }

    void WaterFall::pushFFT()
    {
        if (rawFFTs == NULL)
        {
            return;
        }
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        double offsetRatio = viewOffset / (wholeBandwidth / 2.0);
        int drawDataSize = (viewBandwidth / wholeBandwidth) * rawFFTSize;
        int drawDataStart = (((double)rawFFTSize / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);

        if (waterfallVisible)
        {
            // doZoom(drawDataStart, drawDataSize, dataWidth, &rawFFTs[currentFFTLine * rawFFTSize], latestFFT);
            doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, &rawFFTs[currentFFTLine * rawFFTSize], latestFFT);
            memmove(&waterfallFb[dataWidth], waterfallFb, dataWidth * (waterfallHeight - 1) * sizeof(uint32_t));
            float pixel;
            float dataRange = waterfallMax - waterfallMin;
            for (int j = 0; j < dataWidth; j++)
            {
                pixel = (std::clamp<float>(latestFFT[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;
                int id = (int)(pixel * (WATERFALL_RESOLUTION - 1));
                waterfallFb[j] = waterfallPallet[id];
            }
            waterfallUpdate = true;
        }
        else
        {
            // doZoom(drawDataStart, drawDataSize, dataWidth, rawFFTs, latestFFT);
            doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, rawFFTs, latestFFT);
            fftLines = 1;
        }

        // Apply smoothing if enabled
        if (fftSmoothing && latestFFT != NULL && smoothingBuf != NULL && fftLines != 0)
        {
            std::lock_guard<std::mutex> lck2(smoothingBufMtx);
            volk_32f_s32f_multiply_32f(latestFFT, latestFFT, smoothingAlpha, dataWidth);
            volk_32f_s32f_multiply_32f(smoothingBuf, smoothingBuf, smoothingBeta, dataWidth);
            volk_32f_x2_add_32f(smoothingBuf, latestFFT, smoothingBuf, dataWidth);
            memcpy(latestFFT, smoothingBuf, dataWidth * sizeof(float));
        }

        if (selectedVFO != "" && vfos.size() > 0)
        {
            float dummy;
            snrSmoothing = true; // DMH TEST
            if (snrSmoothing)
            {
                float newSNR = 0.0f;
                calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, vfos[selectedVFO], dummy, newSNR);
                selectedVFOSNR = (snrSmoothingBeta * selectedVFOSNR) + (snrSmoothingAlpha * newSNR);
            }
            else
            {
                calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, vfos[selectedVFO], dummy, selectedVFOSNR);
            }
        }

        // If FFT hold is enabled, update it
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0)
        {
            for (int i = 1; i < dataWidth; i++)
            {
                latestFFTHold[i] = std::max<float>(latestFFT[i], latestFFTHold[i] - fftHoldSpeed);
            }
        }

        buf_mtx.unlock();
    }

    void WaterFall::updatePallette(float colors[][3], int colorCount)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++)
        {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[lowerId][0] * (1.0 - ratio)) + (colors[upperId][0] * (ratio));
            float g = (colors[lowerId][1] * (1.0 - ratio)) + (colors[upperId][1] * (ratio));
            float b = (colors[lowerId][2] * (1.0 - ratio)) + (colors[upperId][2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    void WaterFall::updatePalletteFromArray(float *colors, int colorCount)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++)
        {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[(lowerId * 3) + 0] * (1.0 - ratio)) + (colors[(upperId * 3) + 0] * (ratio));
            float g = (colors[(lowerId * 3) + 1] * (1.0 - ratio)) + (colors[(upperId * 3) + 1] * (ratio));
            float b = (colors[(lowerId * 3) + 2] * (1.0 - ratio)) + (colors[(upperId * 3) + 2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    void WaterFall::autoRange()
    {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        float min = INFINITY;
        float max = -INFINITY;
        for (int i = 0; i < dataWidth; i++)
        {
            if (latestFFT[i] < min)
            {
                min = latestFFT[i];
            }
            if (latestFFT[i] > max)
            {
                max = latestFFT[i];
            }
        }
        fftMin = min - 5;
        fftMax = max + 5;
    }

    void WaterFall::setCenterFrequency(double freq)
    {
        // flog::info("void WaterFall::setCenterFrequency(double freq) {0}", freq);
        centerFreq = freq;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        updateAllVFOs();
        // flog::info("void WaterFall::setCenterFrequency(double freq) {0}", freq);
    }

    double WaterFall::getCenterFrequency()
    {
        return centerFreq;
    }

    void WaterFall::setBandwidth(double bandWidth)
    {
        double currentRatio = viewBandwidth / wholeBandwidth;
        wholeBandwidth = bandWidth;
        setViewBandwidth(bandWidth * currentRatio);
        for (auto const &[name, vfo] : vfos)
        {
            if (vfo->lowerOffset < -(bandWidth / 2))
            {
                vfo->setCenterOffset(-(bandWidth / 2));
            }
            if (vfo->upperOffset > (bandWidth / 2))
            {
                vfo->setCenterOffset(bandWidth / 2);
            }
        }
        updateAllVFOs();
    }
    /*
    void WaterFall::setBandwidth(double bandWidth)
    {
        double currentRatio = viewBandwidth / wholeBandwidth;
        wholeBandwidth = bandWidth;
        double finalBw = bandWidth * currentRatio;
        // core::configManager.acquire();
        std::string _source = core::configManager.conf["source"];
        // core::configManager.release();
        // std::string _source = "Airspy";
        flog::info("WaterFall::setBandwidth {0}, bandWidth {1}, viewBandwidth {2}, finalBw {3}, _source {4}", currentRatio, bandWidth, viewBandwidth, finalBw, _source);

        if (_source == "Airspy" || _source == "ARM")
        {
            if (finalBw > VIEWBANDWICH)
                finalBw = VIEWBANDWICH;
        }
        setViewBandwidth(finalBw);

        for (auto const &[name, vfo] : vfos)
        {
            if (vfo->lowerOffset < -(bandWidth / 2))
            {
                vfo->setCenterOffset(-(bandWidth / 2));
            }
            if (vfo->upperOffset > (bandWidth / 2))
            {
                vfo->setCenterOffset(bandWidth / 2);
            }
        }
        updateAllVFOs();
    }
    */
    double WaterFall::getBandwidth()
    {
        return wholeBandwidth;
    }

    void WaterFall::setViewBandwidth(double bandWidth)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (bandWidth == viewBandwidth)
        {
            return;
        }
        if (abs(viewOffset) + (bandWidth / 2.0) > wholeBandwidth / 2.0)
        {
            if (viewOffset < 0)
            {
                viewOffset = (bandWidth / 2.0) - (wholeBandwidth / 2.0);
            }
            else
            {
                viewOffset = (wholeBandwidth / 2.0) - (bandWidth / 2.0);
            }
        }
        viewBandwidth = bandWidth;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        // flog::info("void WaterFall::setViewBandwidth viewBandwidth {0}, lowerFreq {1}, upperFreq {2}", bandWidth, lowerFreq, upperFreq);

        range = findBestRange(bandWidth, maxHSteps);
        if (_fullUpdate)
        {
            updateWaterfallFb();
        };
        updateAllVFOs();
    }

    double WaterFall::getViewBandwidth()
    {
        return viewBandwidth;
    }

    void WaterFall::setViewOffset(double offset)
    {
        // flog::error("setViewOffset ({0})", offset);
        // gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (offset == viewOffset)
        {
            return;
        }
        if (offset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0))
        {
            offset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
        }
        if (offset + (viewBandwidth / 2.0) > (wholeBandwidth / 2.0))
        {
            offset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
        }
        viewOffset = offset;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        if (_fullUpdate)
        {
            updateWaterfallFb();
        };
        updateAllVFOs();
    }

    double WaterFall::getViewOffset()
    {
        return viewOffset;
    }

    void WaterFall::setFFTMin(float min)
    {
        fftMin = min;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMin()
    {
        return fftMin;
    }

    void WaterFall::setFFTMax(float max)
    {
        fftMax = max;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMax()
    {
        return fftMax;
    }

    void WaterFall::setFullWaterfallUpdate(bool fullUpdate)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        _fullUpdate = fullUpdate;
    }

    void WaterFall::setWaterfallMin(float min)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (min == waterfallMin)
        {
            return;
        }
        waterfallMin = min;
        if (_fullUpdate)
        {
            updateWaterfallFb();
        };
    }

    float WaterFall::getWaterfallMin()
    {
        return waterfallMin;
    }

    void WaterFall::setWaterfallMax(float max)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (max == waterfallMax)
        {
            return;
        }
        waterfallMax = max;
        if (_fullUpdate)
        {
            updateWaterfallFb();
        };
    }

    float WaterFall::getWaterfallMax()
    {
        return waterfallMax;
    }

    void WaterFall::updateAllVFOs(bool checkRedrawRequired)
    {
        for (auto const &[name, vfo] : vfos)
        {
            if (selectedVFO != "Канал приймання" || selectedVFO != name)
            {
                continue;
            }
            if (checkRedrawRequired && !vfo->redrawRequired)
            {
                continue;
            }
            vfo->updateDrawingVars(viewBandwidth, dataWidth, viewOffset, widgetPos, fftHeight);
            vfo->wfRectMin = ImVec2(vfo->rectMin.x, wfMin.y);
            vfo->wfRectMax = ImVec2(vfo->rectMax.x, wfMax.y);
            vfo->wfLineMin = ImVec2(vfo->lineMin.x, wfMin.y - 1);
            vfo->wfLineMax = ImVec2(vfo->lineMax.x, wfMax.y - 1);
            vfo->wfLbwSelMin = ImVec2(vfo->wfRectMin.x - 2, vfo->wfRectMin.y);
            vfo->wfLbwSelMax = ImVec2(vfo->wfRectMin.x + 2, vfo->wfRectMax.y);
            vfo->wfRbwSelMin = ImVec2(vfo->wfRectMax.x - 2, vfo->wfRectMin.y);
            vfo->wfRbwSelMax = ImVec2(vfo->wfRectMax.x + 2, vfo->wfRectMax.y);
            vfo->redrawRequired = false;
        }
    }

    void WaterFall::setRawFFTSize(int size)
    {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        rawFFTSize = size;
        int wfSize = std::max<int>(1, waterfallHeight);
        if (rawFFTs != NULL)
        {
            rawFFTs = (float *)realloc(rawFFTs, rawFFTSize * wfSize * sizeof(float));
        }
        else
        {
            rawFFTs = (float *)malloc(rawFFTSize * wfSize * sizeof(float));
        }
        fftLines = 0;
        memset(rawFFTs, 0, rawFFTSize * waterfallHeight * sizeof(float));
        updateWaterfallFb();
    }

    void WaterFall::setBandPlanPos(int pos)
    {
        bandPlanPos = pos;
    }

    void WaterFall::setFFTHold(bool hold)
    {
        fftHold = hold;
        if (fftHold && latestFFTHold)
        {
            for (int i = 0; i < dataWidth; i++)
            {
                latestFFTHold[i] = -1000.0;
            }
        }
    }

    void WaterFall::setFFTHoldSpeed(float speed)
    {
        fftHoldSpeed = speed;
    }

    void WaterFall::setFFTSmoothing(bool enabled)
    {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        fftSmoothing = enabled;

        // Free buffer if not null
        if (smoothingBuf)
        {
            delete[] smoothingBuf;
        }

        // If disabled, stop here
        if (!enabled)
        {
            smoothingBuf = NULL;
            return;
        }

        // Allocate and copy existing FFT into it
        smoothingBuf = new float[dataWidth];
        if (latestFFT)
        {
            std::lock_guard<std::recursive_mutex> lck2(latestFFTMtx);
            memcpy(smoothingBuf, latestFFT, dataWidth * sizeof(float));
        }
        else
        {
            memset(smoothingBuf, 0, dataWidth * sizeof(float));
        }
    }

    void WaterFall::setFFTSmoothingSpeed(float speed)
    {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        smoothingAlpha = speed;
        smoothingBeta = 1.0f - speed;
    }

    void WaterFall::setSNRSmoothing(bool enabled)
    {
        snrSmoothing = enabled;
    }

    void WaterFall::setSNRSmoothingSpeed(float speed)
    {
        snrSmoothingAlpha = speed;
        snrSmoothingBeta = 1.0f - speed;
    }
    float *WaterFall::acquireLatestFFT(int &width)
    {
        latestFFTMtx.lock();
        if (!latestFFT)
        {
            latestFFTMtx.unlock();
            return NULL;
        }
        width = dataWidth;
        return latestFFT;
    }

    void WaterFall::releaseLatestFFT()
    {
        latestFFTMtx.unlock();
    }

    void WaterFall::setSource(std::string source)
    {
        sourceName = source;
    }
    std::string WaterFall::getSource()
    {
        return sourceName;
    };

    void WaterFall::setPrevCenterFrequency(double val)
    {
        prevCenterFrequency = val;
    };
    double WaterFall::getPrevCenterFrequency()
    {
        return prevCenterFrequency;
    };

    void WaterfallVFO::setOffset(double offset)
    {
        // flog::info("___setOffset ({0})", offset);
        generalOffset = offset;
        // if(!mainVFO)  return;
        if (reference == REF_CENTER)
        {
            centerOffset = offset;
            lowerOffset = offset - (bandwidth / 2.0);
            upperOffset = offset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER)
        {
            lowerOffset = offset;
            centerOffset = offset + (bandwidth / 2.0);
            upperOffset = offset + bandwidth;
        }
        else if (reference == REF_UPPER)
        {
            upperOffset = offset;
            centerOffset = offset - (bandwidth / 2.0);
            lowerOffset = offset - bandwidth;
        }
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setCenterOffset(double offset)
    {
        // flog::info("void WaterfallVFO::setCenterOffset(double offset) {0}", offset);

        if (reference == REF_CENTER)
        {
            generalOffset = offset;
        }
        else if (reference == REF_LOWER)
        {
            generalOffset = offset - (bandwidth / 2.0);
        }
        else if (reference == REF_UPPER)
        {
            generalOffset = offset + (bandwidth / 2.0);
        }
        centerOffset = offset;
        lowerOffset = offset - (bandwidth / 2.0);
        upperOffset = offset + (bandwidth / 2.0);
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setBandwidth(double bw)
    {
        /// flog::info("setBandwidth ({0})", bw);

        if (bandwidth == bw || bw < 0)
        {
            return;
        }
        bandwidth = bw;
        if (reference == REF_CENTER)
        {
            lowerOffset = centerOffset - (bandwidth / 2.0);
            upperOffset = centerOffset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER)
        {
            centerOffset = lowerOffset + (bandwidth / 2.0);
            upperOffset = lowerOffset + bandwidth;
            centerOffsetChanged = true;
        }
        else if (reference == REF_UPPER)
        {
            centerOffset = upperOffset - (bandwidth / 2.0);
            lowerOffset = upperOffset - bandwidth;
            centerOffsetChanged = true;
        }
        bandwidthChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setReference(int ref)
    {
        flog::info("setReference ({0}), generalOffset {1} ", ref, generalOffset);
        if (reference == ref || ref < 0 || ref >= _REF_COUNT)
        {
            return;
        }
        reference = ref;
        setOffset(generalOffset);

        // if (gui::waterfall.selectedVFO == "Канал приймання")
        //  else
        //     setOffset(generalOffset, false);
    }

    void WaterfallVFO::setNotchOffset(double offset)
    {
        notchOffset = offset;
        redrawRequired = true;
    }

    void WaterfallVFO::setNotchVisible(bool visible)
    {
        notchVisible = visible;
        redrawRequired = true;
    }

    void WaterfallVFO::updateDrawingVars(double viewBandwidth, float dataWidth, double viewOffset, ImVec2 widgetPos, int fftHeight)
    {
        int center = roundf((((centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int left = roundf((((lowerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int right = roundf((((upperOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int notch = roundf((((notchOffset + centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));

        // Check weather the line is visible
        if (left >= 0 && left < dataWidth && reference == REF_LOWER)
        {
            lineVisible = true;
        }
        else if (center >= 0 && center < dataWidth && reference == REF_CENTER)
        {
            lineVisible = true;
        }
        else if (right >= 0 && right < dataWidth && reference == REF_UPPER)
        {
            lineVisible = true;
        }
        else
        {
            lineVisible = false;
        }

        // Calculate the position of the line
        if (reference == REF_LOWER)
        {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_CENTER)
        {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_UPPER)
        {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMax.y - 1);
        }

        int _left = left;
        int _right = right;
        left = std::clamp<int>(left, 0, dataWidth - 1);
        right = std::clamp<int>(right, 0, dataWidth - 1);
        leftClamped = (left != _left);
        rightClamped = (right != _right);

        rectMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y + 1);
        rectMax = ImVec2(gui::waterfall.fftAreaMin.x + right + 1, gui::waterfall.fftAreaMax.y);

        float gripSize = 2.0f * style::uiScale;
        lbwSelMin = ImVec2(rectMin.x - gripSize, rectMin.y);
        lbwSelMax = ImVec2(rectMin.x + gripSize, rectMax.y);
        rbwSelMin = ImVec2(rectMax.x - gripSize, rectMin.y);
        rbwSelMax = ImVec2(rectMax.x + gripSize, rectMax.y);

        notchMin = ImVec2(gui::waterfall.fftAreaMin.x + notch - gripSize, gui::waterfall.fftAreaMin.y);
        notchMax = ImVec2(gui::waterfall.fftAreaMin.x + notch + gripSize, gui::waterfall.fftAreaMax.y - 1);
    }

    void WaterfallVFO::draw(ImGuiWindow *window, bool selected)
    {
        window->DrawList->AddRectFilled(rectMin, rectMax, color);
        // flog::info("rectMin.x {0}, rectMin.y {1}, rectMax.x {2}, rectMax.y {3}", rectMin.x, rectMin.y, rectMax.x, rectMax.y);
        if (lineVisible)
        {
            window->DrawList->AddLine(lineMin, lineMax, selected ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::uiScale);
        }

        if (notchVisible)
        {
            window->DrawList->AddRectFilled(notchMin, notchMax, IM_COL32(255, 0, 0, 127));
        }

        if (!gui::mainWindow.lockWaterfallControls && !gui::waterfall.inputHandled)
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            // flog::info("mousePos.x {0}, mousePos.y {1}, rectMax.x {2}, rectMin.x {3}", mousePos.x, mousePos.y, rectMax.x, rectMin.x);
            if (rectMax.x - rectMin.x < 10)
            {
                return;
            }
            if (reference != REF_LOWER && !bandwidthLocked && !leftClamped)
            {
                if (IS_IN_AREA(mousePos, lbwSelMin, lbwSelMax))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                else if (IS_IN_AREA(mousePos, wfLbwSelMin, wfLbwSelMax))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
            if (reference != REF_UPPER && !bandwidthLocked && !rightClamped)
            {
                if (IS_IN_AREA(mousePos, rbwSelMin, rbwSelMax))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                else if (IS_IN_AREA(mousePos, wfRbwSelMin, wfRbwSelMax))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
        }
    };

    void WaterFall::showWaterfall()
    {
        buf_mtx.lock();
        if (rawFFTs == NULL)
        {
            flog::error("Null rawFFT");
        }
        waterfallVisible = true;
        onResize();
        memset(rawFFTs, 0, waterfallHeight * rawFFTSize * sizeof(float));
        updateWaterfallFb();
        buf_mtx.unlock();
    }

    void WaterFall::hideWaterfall()
    {
        buf_mtx.lock();
        waterfallVisible = false;
        onResize();
        buf_mtx.unlock();
    }

    void WaterFall::showFFT()
    {
        buf_mtx.lock();
        if (rawFFTs == NULL)
        {
            flog::error("Null rawFFT");
        }
        fftVisible = true;
        onResize();
        memset(rawFFTs, 0, waterfallHeight * rawFFTSize * sizeof(float));
        // updateWaterfallFb();
        buf_mtx.unlock();
    }

    void WaterFall::hideFFT()
    {
        buf_mtx.lock();
        fftVisible = false;
        onResize();
        buf_mtx.unlock();
    }
    void WaterFall::setFFTHeight(int height)
    {
        FFTAreaHeight = height;
        newFFTAreaHeight = height;
        buf_mtx.lock();
        onResize();
        buf_mtx.unlock();
    }

    int WaterFall::getFFTHeight()
    {
        return FFTAreaHeight;
    }

    void WaterFall::showBandplan()
    {
        bandplanVisible = true;
    }

    void WaterFall::hideBandplan()
    {
        bandplanVisible = false;
    }

    void WaterfallVFO::setSnapInterval(double interval)
    {
        snapInterval = interval;
    }

};
