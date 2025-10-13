#include <gui/menus/display.h>
#include <gui/menus/theme.h>
#include <imgui.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/colormaps.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <gui/widgets/folder_select.h>
#include <algorithm>

namespace displaymenu
{
    bool showWaterfall;
    bool fullWaterfallUpdate = true;
    int colorMapId = 0;
    std::vector<std::string> colorMapNames;
    std::string colorMapNamesTxt = "";
    std::string colorMapAuthor = "";
    int selectedWindow = 0;
    int fftRate = 20;
    int uiScaleId = 0;
    bool restartRequired = false;
    bool fftHold = false;
    int fftHoldSpeed = 60;
    bool fftSmoothing = false;
    int fftSmoothingSpeed = 100;
    bool snrSmoothing = false;
    int snrSmoothingSpeed = 20;

    int radioMode = 0;
    bool saveInDir = false;
    // DMH +++
    char hostname[1024] = "localhost";
    int port = 4000;
    bool Admin = false;
    int maxRecDuration = 5;
    int baseband_band = 1000000;

    int InstanceNum = 1;
    std::string InstanceName = "Aster";
    std::string jsonPath = "%ROOT%/Banks";
    std::string wavPath = "%ROOT%/recordings";
    std::string Url = "http://localhost:18101/event";

    int themeId;
    std::vector<std::string> themeNames;
    std::string themeNamesTxt;
    
    OptionList<int, int> fftSizes;
    OptionList<float, float> uiScales;

    const int FFTSizes[] = {
        //        524288,
        //        262144,
        //        131072,
        65536,
        32768,
        16384,
        8192,
        4096,
        2048,
        1024};
    // //131072 "524288\0" "262144\0"
    const char *FFTSizesStr = "65536\0"
                              "32768\0"
                              "16384\0"
                              "8192\0"
                              "4096\0"
                              "2048\0"
                              "1024\0";

    int fftSizeId = 0;

    enum
    {
        MODE_ASTER,
        MODE_MALVA,
        MODE_AZALIY
    };

    const IQFrontEnd::FFTWindow fftWindowList[] = {
        IQFrontEnd::FFTWindow::RECTANGULAR,
        IQFrontEnd::FFTWindow::BLACKMAN,
        IQFrontEnd::FFTWindow::NUTTALL,
        IQFrontEnd::FFTWindow::HANN};

    void updateFFTSpeeds()
    {
        gui::waterfall.setFFTHoldSpeed((float)fftHoldSpeed / ((float)fftRate * 10.0f));
        gui::waterfall.setFFTSmoothingSpeed(std::min<float>((float)fftSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
        gui::waterfall.setSNRSmoothingSpeed(std::min<float>((float)snrSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
    }
    FolderSelect *folderSelectJson;
    FolderSelect *folderSelectWav;

    bool getFFTHold()
    {
        return fftHold;
    };
    void setFFTHold(bool ffthold)
    {
        fftHold = ffthold;
    };

    bool getWaterfall()
    {
        return showWaterfall;
    };
    void setWaterfall(bool waterfall)
    {
        showWaterfall = waterfall;
    };

    void init()
    {
        // Define FFT sizes
        /*
        fftSizes.define(524288, "524288", 524288);
        fftSizes.define(262144, "262144", 262144);
        fftSizes.define(131072, "131072", 131072);
        fftSizes.define(65536, "65536", 65536);
        fftSizes.define(32768, "32768", 32768);
        fftSizes.define(16384, "16384", 16384);
        fftSizes.define(8192, "8192", 8192);
        fftSizes.define(4096, "4096", 4096);
        fftSizes.define(2048, "2048", 2048);
        fftSizes.define(1024, "1024", 1024); 
        fftSizeId = fftSizes.valueId(65536);
        int size = core::configManager.conf["fftSize"];
        if (fftSizes.keyExists(size)) {
            fftSizeId = fftSizes.keyId(size);
        }
        sigpath::iqFrontEnd.setFFTSize(fftSizes.value(fftSizeId));
        */
        core::configManager.acquire();
        showWaterfall = core::configManager.conf["showWaterfall"];
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        std::string colormapName = core::configManager.conf["colorMap"];

        fullWaterfallUpdate = core::configManager.conf["fullWaterfallUpdate"];

        fftSizeId = 3;
        int fftSize = core::configManager.conf["fftSize"];

        fftRate = core::configManager.conf["fftRate"];

        selectedWindow = std::clamp<int>((int)core::configManager.conf["fftWindow"], 0, (sizeof(fftWindowList) / sizeof(IQFrontEnd::FFTWindow)) - 1);

        Admin = false;
        try
        {
            Admin = (bool)core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            Admin = 0;
        }

        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"]; // std::clamp<int>((int)core::configManager.conf["RadioMode"], 0, 1);
        }
        catch (const std::exception &e)
        {
            // std::cerr << e.what() << '\n';
            radioMode = 0;
        }

        try
        {
            saveInDir = core::configManager.conf["SaveInDirMalva"];
        }
        catch (const std::exception &e)
        {
            saveInDir = false;
            core::configManager.conf["SaveInDirMalva"] = saveInDir;
        }
        if (radioMode == 0)
            saveInDir = false;

        gui::menu.locked = core::configManager.conf["lockMenuOrder"];

        fftHold = core::configManager.conf["fftHold"];
        fftHoldSpeed = core::configManager.conf["fftHoldSpeed"];
        fftSmoothing = core::configManager.conf["fftSmoothing"];
        fftSmoothingSpeed = core::configManager.conf["fftSmoothingSpeed"];
        // DMH +++
        bool _update = false;
        try
        {
            maxRecDuration = core::configManager.conf["maxRecDuration"];
        }
        catch (...)
        {
            maxRecDuration = 5;
            core::configManager.conf["maxRecDuration"] = maxRecDuration;
            _update = true;
        }

        std::string hostStr = "localhost";
        try
        {
            hostStr = core::configManager.conf["hostname"];
            strcpy(hostname, hostStr.c_str());
        }
        catch (...)
        {
            std::cout << "Error config.conf 172 " << std::endl;
            core::configManager.conf["hostname"] = "    localhost";
            hostStr = "localhost";
            _update = true;
        }
        strcpy(hostname, hostStr.c_str());
        try
        {
            port = core::configManager.conf["port"];
        }
        catch (...)
        {
            std::cout << "Error config.conf 183 " << std::endl;
            core::configManager.conf["port"] = 400;
            port = 400;
            _update = true;
        }

        try
        {
            InstanceName = core::configManager.conf["InstanceName"];
            InstanceNum = core::configManager.conf["InstanceNum"];
        }
        catch (...)
        {
            InstanceName = "Aster";
            std::cout << "Error config.conf M1" << std::endl;
        }
        try
        {
            jsonPath = core::configManager.conf["PathJson"];
        }
        catch (...)
        {
            jsonPath = "%ROOT%/Banks";
            std::cout << "Error config.conf M1" << std::endl;
        }
        try
        {
            wavPath = core::configManager.conf["PathWav"];
        }
        catch (...)
        {
            wavPath = "%ROOT%/recordings";
            std::cout << "Error config.conf M1" << std::endl;
        }

        folderSelectWav = new FolderSelect(wavPath);
        folderSelectJson = new FolderSelect(jsonPath);
        try
        {
            Url = core::configManager.conf["Url"];
        }
        catch (...)
        {
            Url = "http://localhost:18101/event";
            std::cout << "Error config.conf Url" << std::endl;
        }
        // dmh

        // theme
        std::string resDir = core::configManager.conf["resourcesDirectory"];
        std::string selectedThemeName = core::configManager.conf["theme"];
        if (_update)
            core::configManager.release(true);
        else
            core::configManager.release();

        //-------------------------
        
        for (int i = 0; i < 7; i++)
        {
            if (fftSize == FFTSizes[i])
            {
                fftSizeId = i;
                break;
            }
        }
        
        if (colormaps::maps.find(colormapName) != colormaps::maps.end())
        {
            colormaps::Map map = colormaps::maps[colormapName];
            gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
        }

        for (auto const &[name, map] : colormaps::maps)
        {
            colorMapNames.push_back(name);
            colorMapNamesTxt += name;
            colorMapNamesTxt += '\0';
            if (name == colormapName)
            {
                colorMapId = (colorMapNames.size() - 1);
                colorMapAuthor = map.author;
            }
        }
        gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
        sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId]);
        sigpath::iqFrontEnd.setFFTRate(fftRate);
        sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);

        gui::waterfall.setFFTHold(fftHold);
        gui::waterfall.setFFTSmoothing(fftSmoothing);
        updateFFTSpeeds();

        // Define and load UI scales
        uiScales.define(1.0f, "100%", 1.0f);
        uiScales.define(2.0f, "200%", 2.0f);
        uiScales.define(3.0f, "300%", 3.0f);
        uiScales.define(4.0f, "400%", 4.0f);
        uiScaleId = uiScales.valueId(style::uiScale);

        gui::themeManager.loadThemesFromDir(resDir + "/themes/");

        // Select theme by name, if not available, apply Dark theme
        themeNames = gui::themeManager.getThemeNames();
        auto it = std::find(themeNames.begin(), themeNames.end(), selectedThemeName);
        if (it == themeNames.end())
        {
            it = std::find(themeNames.begin(), themeNames.end(), "Темна");
            selectedThemeName = "Темна";
        }
        themeId = std::distance(themeNames.begin(), it);
        gui::themeManager.applyTheme(themeNames[themeId]);

        themeNamesTxt = "";
        for (auto name : themeNames)
        {
            themeNamesTxt += name;
            themeNamesTxt += '\0';
        }
    }

    void setWaterfallShown(bool shown) {
        showWaterfall = shown;
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        core::configManager.acquire();
        core::configManager.conf["showWaterfall"] = showWaterfall;
        core::configManager.release(true);
    }

    void checkKeybinds() {
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            setWaterfallShown(!showWaterfall);
        }
    }

    void draw(void *ctx)
    {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool homePressed = ImGui::IsKeyPressed(ImGuiKey_Home, false);
        // DMH +++ <<<
        char nameInstance[1024];
        strcpy(nameInstance, InstanceName.c_str());
        char nameUrl[1024];
        strcpy(nameUrl, Url.c_str());

        if (Admin)
        {

            ImGui::LeftLabel("Назва  екземпляру ПЗ");
            ImGui::FillWidth();
            if (ImGui::InputText("##aster_instance_name", nameInstance, 1024))
            {
                try
                {
                    InstanceName = nameInstance;
                    core::configManager.acquire();
                    core::configManager.conf["InstanceName"] = InstanceName;
                    core::configManager.release(true);
                }
                catch (...)
                {
                    std::cout << "Error config.conf 289 " << std::endl;
                }
            }

            ImGui::LeftLabel("Номер екземпляру ПЗ");
            ImGui::FillWidth();
            if (ImGui::InputInt("##aster_instance_num", &InstanceNum))
            {
                updateFFTSpeeds();
                core::configManager.acquire();
                core::configManager.conf["InstanceNum"] = InstanceNum;
                core::configManager.release(true);
            }

            ImGui::LeftLabel("URL бази даних ");
            if (ImGui::InputText("##aster_Url", nameUrl, 1023))
            {
                try
                {
                    core::configManager.acquire();
                    core::configManager.conf["Url"] = std::string(nameUrl);
                    core::configManager.release(true);
                }
                catch (...)
                {
                    std::cout << "Error config.conf Url" << std::endl;
                }
            }

            if (ImGui::InputText("##aster_srv_srv_host", hostname, 1023))
            {
                try
                {
                    core::configManager.acquire();
                    core::configManager.conf["hostname"] = std::string(hostname);
                    core::configManager.release(true);
                }
                catch (...)
                {
                    std::cout << "Error config.conf 321" << std::endl;
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##aster_srv_srv0_port", &port, 0, 0))
            {
                try
                {
                    core::configManager.acquire();
                    core::configManager.conf["port"] = port;
                    core::configManager.release(true);
                }
                catch (...)
                {
                    std::cout << "Error config.conf 333" << std::endl;
                }
            }
        }
        // dmh
        if (Admin)
        {
            if (ImGui::Checkbox("Водоспад##_sdrpp", &showWaterfall) || homePressed)
            {
                if (homePressed)
                {
                    showWaterfall = !showWaterfall;
                }
                showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
                core::configManager.acquire();
                core::configManager.conf["showWaterfall"] = showWaterfall;
                core::configManager.release(true);
            }

            if (ImGui::Checkbox("Повне оновлення водоспаду##_sdrpp", &fullWaterfallUpdate))
            {
                gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
                core::configManager.acquire();
                core::configManager.conf["fullWaterfallUpdate"] = fullWaterfallUpdate;
                core::configManager.release(true);
            }

            if (ImGui::Checkbox("Блокування переміщення модулів##_sdrpp", &gui::menu.locked))
            {
                core::configManager.acquire();
                core::configManager.conf["lockMenuOrder"] = gui::menu.locked;
                core::configManager.release(true);
            }

            if (ImGui::Checkbox("Утримання макс. рівня##_sdrpp", &fftHold))
            {
                gui::waterfall.setFFTHold(fftHold);
                core::configManager.acquire();
                core::configManager.conf["fftHold"] = fftHold;
                core::configManager.release(true);
            }

            if (ImGui::Checkbox("Зглажування спектру##_sdrpp", &fftSmoothing))
            {
                gui::waterfall.setFFTSmoothing(fftSmoothing);
                core::configManager.acquire();
                core::configManager.conf["fftSmoothing"] = fftSmoothing;
                core::configManager.release(true);
            }

            ImGui::LeftLabel("Частота утримання макс.");
            ImGui::FillWidth();
            if (ImGui::InputInt("##sdrpp_fft_hold_speed", &fftHoldSpeed))
            {
                updateFFTSpeeds();
                core::configManager.acquire();
                core::configManager.conf["fftHoldSpeed"] = fftHoldSpeed;
                core::configManager.release(true);
            }

            ImGui::LeftLabel("Швидкість зглажування");
            ImGui::FillWidth();
            if (ImGui::InputInt("##sdrpp_fft_smoothing_speed", &fftSmoothingSpeed))
            {
                fftSmoothingSpeed = std::max<int>(fftSmoothingSpeed, 1);
                updateFFTSpeeds();
                core::configManager.acquire();
                core::configManager.conf["fftSmoothingSpeed"] = fftSmoothingSpeed;
                core::configManager.release(true);
            }
            /*
                    ImGui::LeftLabel("Масш. розд. здатності");
                    ImGui::FillWidth();
                    if (ImGui::Combo("##sdrpp_ui_scale", &uiScaleId, uiScales.txt)) {
                        core::configManager.acquire();
                        core::configManager.conf["uiScale"] = uiScales[uiScaleId];
                        core::configManager.release(true);
                        restartRequired = true;
                    }
            */
            ImGui::LeftLabel("Частота кадрів");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##sdrpp_fft_rate", &fftRate, 1, 24))
            {
                fftRate = std::clamp<int>(fftRate, 1, 24);
                // fftRate = std::max<int>(1, fftRate);
                sigpath::iqFrontEnd.setFFTRate(fftRate);
                updateFFTSpeeds();
                core::configManager.acquire();
                core::configManager.conf["fftRate"] = fftRate;
                core::configManager.release(true);
            }

            ImGui::LeftLabel("Розрізнення (точок)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo("##sdrpp_fft_size", &fftSizeId, FFTSizesStr))
            {
                sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId]);
                core::configManager.acquire();
                core::configManager.conf["fftSize"] = FFTSizes[fftSizeId];
                core::configManager.release(true);
            }
            if (Admin)
            {
                ImGui::LeftLabel("Фільтр вікна");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo("##sdrpp_fft_window", &selectedWindow, "Rectangular\0Blackman\0Nuttall\0Nann\0"))
                {
                    sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);
                    core::configManager.acquire();
                    core::configManager.conf["fftWindow"] = selectedWindow;
                    core::configManager.release(true);
                }
            }
        }

        if (colorMapNames.size() > 0)
        {
            ImGui::LeftLabel("Кольорова схема");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo("##_sdrpp_color_map_sel", &colorMapId, colorMapNamesTxt.c_str()))
            {
                colormaps::Map map = colormaps::maps[colorMapNames[colorMapId]];
                gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
                core::configManager.acquire();
                core::configManager.conf["colorMap"] = colorMapNames[colorMapId];
                core::configManager.release(true);
                colorMapAuthor = map.author;
            }
            // ImGui::Text(" Кольорова схема Автор: %s", colorMapAuthor.c_str());
        }

        ImGui::LeftLabel("Тема");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##aster_theme_select_combo", &themeId, themeNamesTxt.c_str()))
        {
            gui::themeManager.applyTheme(themeNames[themeId]);
            core::configManager.acquire();
            core::configManager.conf["theme"] = themeNames[themeId];
            core::configManager.release(true);
        }
        if (Admin)
        {
            ImGui::Text("Тип РДП:            ");
            ImGui::BeginGroup();
            ImGui::Columns(3, "RadioModeColumns##_", false);
            // ImGui::FillWidth();
            if (ImGui::RadioButton("Айстра##aster_mode_0", radioMode == MODE_ASTER))
            {
                radioMode = MODE_ASTER;
                core::configManager.acquire();
                core::configManager.conf["RadioMode"] = radioMode;
                saveInDir = false;
                core::configManager.conf["SaveInDirMalva"] = saveInDir;
                core::configManager.release(true);
            }
            ImGui::NextColumn();
            if (ImGui::RadioButton("Мальва##aster_mode_1", radioMode == MODE_MALVA))
            {
                radioMode = MODE_MALVA;
                core::configManager.acquire();
                core::configManager.conf["RadioMode"] = radioMode;
                core::configManager.conf["SaveInDirMalva"] = saveInDir;
                core::configManager.release(true);
            }
            ImGui::NextColumn();
            if (ImGui::RadioButton("Азалія##aster_mode_2", radioMode == MODE_AZALIY))
            {
                radioMode = MODE_AZALIY;
                core::configManager.acquire();
                core::configManager.conf["RadioMode"] = radioMode;
                core::configManager.conf["SaveInDirMalva"] = saveInDir;
                core::configManager.release(true);
            }
            ImGui::Columns(1, "EndRadioModeColumns##_", false);
            ImGui::EndGroup();
        }
        if (radioMode == 0)
            style::beginDisabled();
        /*
        if (ImGui::Checkbox("Зберігати записи у папки відповідно до номіналів##_sdrppSave", &saveInDir))
        {

            // gui::waterfall.setFFTSmoothing(saveInDir);
            if (radioMode == MODE_ASTER)
                saveInDir = false;
            core::configManager.acquire();
            core::configManager.conf["SaveInDirMalva"] = saveInDir;
            core::configManager.release(true);
        }
        */
        saveInDir = false;

        if (radioMode == 0)
            style::endDisabled();

        if (!Admin)
            style::beginDisabled();
        {
            ImGui::LeftLabel("Тривалість запису, хв.");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##linger_timeWait", &maxRecDuration, 1, 10))
            { // _waitingTime
                maxRecDuration = std::clamp<int>(maxRecDuration, 1, 10);
                core::configManager.acquire();
                core::configManager.conf["maxRecDuration"] = maxRecDuration;
                core::configManager.release(true);
            }

            ImGui::Text("Шлях до теки для імпорта/експорта файлів");
            ImGui::LeftLabel("    ");
            ImGui::FillWidth();
            if (folderSelectJson->render("##aster_path_json"))
            {
                if (folderSelectJson->pathIsValid())
                {
                    core::configManager.acquire();
                    core::configManager.conf["PathJson"] = folderSelectJson->path;
                    core::configManager.release(true);
                }
            }

            ImGui::Text("Шлях до теки для реєстрації");
            ImGui::LeftLabel("    ");
            ImGui::FillWidth();
            if (folderSelectWav->render("##aster_path_wav_json"))
            {
                if (folderSelectWav->pathIsValid())
                {
                    core::configManager.acquire();
                    core::configManager.conf["PathWav"] = folderSelectWav->path;
                    core::configManager.release(true);
                }
            }
        }
        if (!Admin)
            style::endDisabled();

        if (restartRequired)
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Restart required.");
        }
    }
}
