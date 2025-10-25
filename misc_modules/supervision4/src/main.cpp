#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <thread>
#include <ctm.h>
#include <radio_interface.h>

#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
// #include <portaudio.h>
//  #include <RtAudio.h>
#include "/usr/include/rtaudio/RtAudio.h"

#include <dsp/convert/stereo_to_mono.h>
#include <dsp/sink/ring_buffer.h>
#include <fstream>

#include <core.h>
#include <ctime>
#include <chrono>

#include <gui/menus/source.h>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "utils/wstr.h"
#include "manager.h"
namespace fs = std::filesystem;

#include <curl/curl.h>

#define _DEBUG false
#define MAX_CHANNELS 16
#define CMO 8500000
#define MIN_FREQ 25000000
#define MAX_FREQ 1700000000
// #define MAX_COUNT_OF_LIST 8
#define NUM_MOD 4
#define INTERVAL_FOR_FIND_Threshold 15 // min
#define COUNT_FOR_REFIND_LEVEL 10000
#define COUNT_FOR_REFIND_SKIP 10

SDRPP_MOD_INFO{
    /* Name:            */ "supervision4",
    /* Description:     */ "Multichannel observation manager module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1};

enum
{
    AUDIO_SINK_GET_DEVLIST,
    AUDIO_SINK_CMD_SET_DEV,
    AUDIO_SINK_UPDATE_DEVLIST
};

struct CtrlModeList
{
    char listName[32];
    int sizeOfList;
    int bookmarkName[MAX_COUNT_OF_CTRL_LIST];
    double frequency[MAX_COUNT_OF_CTRL_LIST];
    float bandwidth[MAX_COUNT_OF_CTRL_LIST];
    int mode[MAX_COUNT_OF_CTRL_LIST];
    int level[MAX_COUNT_OF_CTRL_LIST];
    char scard[32];
    int Signal[MAX_COUNT_OF_CTRL_LIST];
};
ConfigManager config;

// #define MAX_LIST_PACKET_SIZE MAX_BM_SIZE * 264 sizeof(CtrlModeList)

enum
{
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char *bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";

std::string getNow()
{
    time_t now = time(0);
    tm *ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%02d/%02d/%02d %02d:%02d:%02d", ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900, ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
    std::string prefix = "";
    return prefix + buf;
}

std::string genLogFileName(std::string prefix)
{
    time_t now = time(0);
    tm *ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%02d-%02d-%02d_%02d-%02d-%02d.log", ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday, ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
    return prefix + buf;
}

class SupervisorModeModule : public ModuleManager::Instance, public TransientBookmarkManager
{
public:
    enum class State
    {
        STOPPED,
        RECEIVING,
        WAITING_FOR_AKF,
        STOPPING
    };

    SupervisorModeModule(std::string name)
    {
        this->name = name;

        maxRecDuration = 5;
        numInstance = 0;
        bool update_conf = false;
        // if (!isARM)
        //{ // Очистка нужна только на сервере/клиенте, но не на АРМ
        //}
        core::configManager.acquire();
        if (core::configManager.conf.contains("USE_curl"))
        {
            use_curl = core::configManager.conf["USE_curl"];
        }
        else
        {
            use_curl = true;
            update_conf = true;
            core::configManager.conf["USE_curl"] = use_curl;
        }
        try
        {
            maxRecDuration = core::configManager.conf["maxRecDuration"];
        }
        catch (const std::exception &e)
        {
            maxRecDuration = 1;
            update_conf = true;
            core::configManager.conf["maxRecDuration"] = maxRecDuration;
        }
        try
        {
            Admin = core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            Admin = false;
        }
        try
        {
            status_AKF = config.conf["status_AKF"];
        }
        catch (const std::exception &e)
        {
            status_AKF = false;
            config.conf["status_AKF"] = status_AKF;
            update_conf = true;
            std::cerr << e.what() << '\n';
        }
        if (core::configManager.conf.contains("SignalIndf") && !core::configManager.conf["SignalIndf"].is_null())
        {
            // Значение существует и не null
            if (core::configManager.conf["SignalIndf"] == true)
                SignalIndf = true;
            else
                SignalIndf = false;
        }
        else
        {
            SignalIndf = false;
            core::configManager.conf["SignalIndf"] = false;
            update_conf = true;
        }
        if (!maxRecDuration)
        {
            maxRecDuration = 1;
            update_conf = true;
            core::configManager.conf["maxRecDuration"] = maxRecDuration;
        }

        try
        {
            numInstance = core::configManager.conf["InstanceNum"];
        }
        catch (const std::exception &e)
        {
            numInstance = 0;
        }

        maxCHANNELS = core::configManager.conf["MAX_CHANNELS"];
        if (maxCHANNELS > MAX_CHANNELS)
        {
            maxCHANNELS = MAX_CHANNELS;
        }
        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
            source = core::configManager.conf["source"];
        }
        catch (const std::exception &e)
        {
            radioMode = 0;
        }
        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];

        try
        {
            if (core::configManager.conf["IsServer"] == true)
                isServer = true;
            else
                isServer = false;

            if (core::configManager.conf["IsARM"] == true)
                isARM = true;
            else
                isARM = false;
        }
        catch (const std::exception &e)
        {
            isServer = false;
            isARM = false;
            std::cerr << e.what() << '\n';
        }
        if (update_conf)
            core::configManager.release(true);
        else
            core::configManager.release();

        clnt_mode = -1; //  || source == "Airspy"

        if (source == "Азалія-сервер")
        {
            clnt_mode = 0;
        }
        if (source == "Азалія-клієнт" || source == "Azalea Client")
        {
            clnt_mode = 1;
        }

        thisInstance = thisInstance + "-4";
        bool _update = false;
        config.acquire();
        std::string selList = config.conf["selectedList"];
        bookmarkDisplayMode = true; // config.conf["bookmarkDisplayMode"];
        // Час очикування
        maxRecWaitTime = 5;
        if (isARM)
        {
            try
            {
                for (int i = 0; i < MAX_SERVERS; i++)
                {
                    if (i == 0)
                        maxRecWaitTime = config.conf["maxRecWaitTime_1"];
                    else if (i == 1)
                        maxRecWaitTime = config.conf["maxRecWaitTime_2"];
                    else if (i == 2)
                        maxRecWaitTime = config.conf["maxRecWaitTime_3"];
                    else if (i == 3)
                        maxRecWaitTime = config.conf["maxRecWaitTime_4"];
                    else if (i == 4)
                        maxRecWaitTime = config.conf["maxRecWaitTime_5"];
                    else if (i == 5)
                        maxRecWaitTime = config.conf["maxRecWaitTime_6"];
                    else if (i == 6)
                        maxRecWaitTime = config.conf["maxRecWaitTime_7"];
                    else if (i == 7)
                        maxRecWaitTime = config.conf["maxRecWaitTime_8"];
                    gui::mainWindow.setMaxRecWaitTime_ctrl(i, maxRecWaitTime);
                }
            }
            catch (const std::exception &e)
            {
                config.conf["maxRecWaitTime_1"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_2"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_3"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_4"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_5"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_6"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_7"] = maxRecWaitTime;
                config.conf["maxRecWaitTime_8"] = maxRecWaitTime;
                _update = true;
                std::cerr << e.what() << '\n';
            }
        }
        else
        {
            try
            {
                maxRecWaitTime = config.conf["maxRecWaitTime"];
                if (maxRecWaitTime == 0)
                    maxRecWaitTime = 5;
            }
            catch (...)
            {
                maxRecWaitTime = 5;
                config.conf["maxRecWaitTime"] = maxRecWaitTime;
                _update = true;
            }
            gui::mainWindow.setMaxRecWaitTime_ctrl(0, maxRecWaitTime);
        }
        if (isARM)
        {
            try
            {
                status_auto_level4 = true;
                for (int i = 0; i < MAX_SERVERS; i++)
                {
                    if (i == 0)
                        status_auto_level4 = config.conf["status_auto_level_1"];
                    else if (i == 1)
                        status_auto_level4 = config.conf["status_auto_level_2"];
                    else if (i == 2)
                        status_auto_level4 = config.conf["status_auto_level_3"];
                    else if (i == 3)
                        status_auto_level4 = config.conf["status_auto_level_4"];
                    else if (i == 4)
                        status_auto_level4 = config.conf["status_auto_level_5"];
                    else if (i == 5)
                        status_auto_level4 = config.conf["status_auto_level_6"];
                    else if (i == 6)
                        status_auto_level4 = config.conf["status_auto_level_7"];
                    else if (i == 7)
                        status_auto_level4 = config.conf["status_auto_level_8"];

                    gui::mainWindow.setAuto_levelCtrl(i, status_auto_level4);
                }
            }
            catch (const std::exception &e)
            {
                status_auto_level4 = false;
                config.conf["status_auto_level_1"] = status_auto_level4;
                config.conf["status_auto_level_2"] = status_auto_level4;
                config.conf["status_auto_level_3"] = status_auto_level4;
                config.conf["status_auto_level_4"] = status_auto_level4;
                config.conf["status_auto_level_5"] = status_auto_level4;
                config.conf["status_auto_level_6"] = status_auto_level4;
                config.conf["status_auto_level_7"] = status_auto_level4;
                config.conf["status_auto_level_8"] = status_auto_level4;
                _update = true;
                std::cerr << e.what() << '\n';
            }
        }
        else
        {
            try
            {
                status_AKF = config.conf["status_AKF"];
            }
            catch (const std::exception &e)
            {
                status_AKF = false;
                config.conf["status_AKF"] = status_AKF;
                _update = true;
            }
            try
            {
                status_auto_level4 = config.conf["status_auto_level"];
            }
            catch (const std::exception &e)
            {
                status_auto_level4 = false;
                config.conf["status_auto_level"] = status_auto_level4;
                _update = true;
                std::cerr << e.what() << '\n';
            }
            gui::mainWindow.setAuto_levelCtrl(0, status_auto_level4);
        }
        config.release(_update);

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks(false);

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);

        root = (std::string)core::args["root"];
        flog::info("name={0}", name);
        for (int i = 0; i < maxCHANNELS; i++)
            ch_recording[i] = false;

        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(2700);
        bandwidthsList.push_back(4000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(220000);
        bandwidthsList.push_back(250000);

        core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);
        core::modComManager.registerInterface("supervision4", name, moduleInterfaceHandler, this);
    }

    ~SupervisorModeModule()
    {
        infoThreadStarted.store(false);
        if (workerInfoThread.joinable())
        {
            workerInfoThread.join(); // Ожидаем завершения потока
        }
        if ((name == "Спостереження" || name == "Спостереженя") && !isARM)
            DelSelectedList(true);
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
        core::modComManager.unregisterInterface(name);
    }

    void postInit()
    {
        currSource = sourcemenu::getCurrSource(); //  sigpath::sourceManager::getCurrSource();
        if (currSource == SOURCE_ARM)
            isARM = true;
        else
            isARM = false;

        flog::info("[4 postInit] postInit currSource {0}, isARM {1}, isServer {2}, name {3}, status_auto_level4 {4}", currSource, isARM, isServer, name, status_auto_level4);

        if (isARM)
        {
            gui::mainWindow.setFirstStart_ctrl(MAX_SERVERS, true);
            gui::mainWindow.setCTRLListNamesTxt(listNamesTxt);
            // gui::mainWindow.setAuto_levelCtrl(MAX_SERVERS, status_auto_level4);
            // gui::mainWindow.setLevelDbCtrl(MAX_SERVERS, curr_level);
            getScanLists();
            uint8_t CurrSrvr = gui::mainWindow.getCurrServer();
            gui::mainWindow.setidOfList_ctrl(CurrSrvr, selectedListId);
            gui::mainWindow.setflag_level_ctrl(CurrSrvr, getflag_level[CurrSrvr]);
            gui::mainWindow.setLevelDbCtrl(CurrSrvr, getgen_level[CurrSrvr]);
            gui::mainWindow.setAKFInd_ctrl(MAX_SERVERS, status_AKF);
            gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
            gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
            // gui::mainWindow.setLevelDbCtrl(MAX_SERVERS, curr_level);

            gui::mainWindow.setUpdateCTRLListForBotton(gui::mainWindow.getCurrServer(), true);
        }
        else
        {
            if (isServer && (name == "Спостереження" || name == "Спостереженя"))
            {
                flog::info("\n CTRL DelSelectedList");
                DelSelectedList(); /// DMH
            }
            gui::mainWindow.setUpdateModule_ctrl(0, false);
        }
        flog::info("\n CTRL currSource {0}, isARM {1}, isServer {2}", currSource, isARM, isServer);

        for (int ch_num = 0; ch_num < MAX_SERVERS; ch_num++)
            channel_states[ch_num].store(State::STOPPED);

        maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_ctrl(gui::mainWindow.getCurrServer());
        // if (isServer || isARM)
        //    workerInfoThread = std::thread(&SupervisorModeModule::workerInfo, this);
    }

    void enable()
    {
        enabled = true;
    }

    void disable()
    {
        enabled = false;
    }

    bool isEnabled()
    {
        return enabled;
    }

private:
    std::atomic<bool> is_stopping{false};

    void updateListAndState(bool _first, bool _update)
    {
        try // <--- Оборачиваем всю операцию в try-catch для максимальной надежности
        {
            // --- Шаг 0: Проверка на наличие списков ---
            if (listNames.empty())
            {
                flog::warn("Cannot change list, because no lists are available.");
                return;
            }

            // --- Шаг 1: Собрать "снимок" состояния GUI в локальные переменные ---
            const uint32_t listIdFromGui = gui::mainWindow.getidOfList_ctrl(CurrSrvr);
            const bool flagLevelFromGui = gui::mainWindow.getflag_level_ctrl(CurrSrvr);
            const int levelDbFromGui = gui::mainWindow.getLevelDbCtrl(CurrSrvr);

            // --- Шаг 2: Определить, какой список будет новым (теперь безопасно) ---
            uint32_t newSelectedId = (listIdFromGui < listNames.size()) ? listIdFromGui : 0;

            const std::string oldSelectedListName = selectedListName;
            const std::string newSelectedListName = listNames[newSelectedId];

            // Пропускаем всю сложную логику, если по факту список не меняется
            if (oldSelectedListName == newSelectedListName && !_update)
            {
                // Если список тот же и нет флага принудительного обновления, выходим
                flog::info("List '{0}' is already selected. No changes needed.", newSelectedListName);
                return;
            }

            flog::info("List change requested: from '{0}' to '{1}' (id={2}).",oldSelectedListName, newSelectedListName, newSelectedId);

            // --- Шаг 3: Атомарно обновить конфигурацию под одной блокировкой ---
            config.acquire();
            // Внутренний try-catch для самой критичной части - работы с JSON
            try
            {
                if (!oldSelectedListName.empty() && oldSelectedListName != newSelectedListName)
                {
                    if (config.conf["lists"].contains(oldSelectedListName))
                    {
                        config.conf["lists"][oldSelectedListName]["showOnWaterfall"] = false;
                    }
                }
                if (config.conf["lists"].contains(newSelectedListName))
                {
                    auto &list = config.conf["lists"][newSelectedListName];
                    list["showOnWaterfall"] = true;
                    list["flaglevel"] = flagLevelFromGui;
                    list["genlevel"] = levelDbFromGui;
                }
                config.conf["selectedList"] = newSelectedListName;
            }
            catch (const nlohmann::json::exception &e)
            {
                flog::error("JSON config update failed: {0}", e.what());
                config.release(); // Освобождаем мьютекс перед выходом из-за ошибки
                throw;            // Пробрасываем исключение, чтобы его поймал внешний catch
            }
            config.release(true);

            // --- Шаг 4: Обновить состояние модуля и GUI ---
            loadByName(newSelectedListName);
            bookmarksUpdated.store(true); // Сигнал для worker()

            getflag_level[CurrSrvr] = flagLevelFromGui;
            getgen_level[CurrSrvr] = levelDbFromGui;

            saveByName(newSelectedListName);
            refreshLists();
            refreshWaterfallBookmarks(false);
            flog::info("State updated for new list '{0}'.", newSelectedListName);

            // --- Шаг 5: Побочные эффекты (тюнинг и т.д.) ---
            if (!_first && currentState.load() == ModuleState::STOPPED)
            {
                double curr_freq = 0;
                if (!bookmarks.empty())
                {
                    curr_freq = bookmarks.begin()->second.frequency;
                }
                if (curr_freq > 0)
                {
                    if (!gui::waterfall.selectedVFO.empty())
                        tuner::centerTuning(gui::waterfall.selectedVFO, curr_freq);
                    tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, gui::freqSelect.frequency);
                    gui::waterfall.centerFreqMoved = true;
                    gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
                }
                _first = false;
            }
            gui::mainWindow.setFirstConn(NUM_MOD, _first);
        }
        catch (const std::exception &e)
        {
            // Ловим любые исключения из шагов 3, 4, 5
            flog::error("A critical error occurred during list update process: {0}. Operation aborted.", e.what());
        }
    }

    static void workerInfo(void *ctx)
    {
        SupervisorModeModule *_this = (SupervisorModeModule *)ctx;
        // if (currSource != SOURCE_ARM) return;
        bool _first = true;
        while (_this->infoThreadStarted.load())
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                flog::warn("if (core::g_isExiting)");
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            {
                std::lock_guard<std::mutex> lck(_this->classMtx);
                if (_this->listUpdater.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    flog::warn("if (_this->listUpdater.load())");
                    continue;
                }

                // flog::info("gui::mainWindow.getUpdateMenuRcv7Ctrl() {0}, _this->isServer {1}", gui::mainWindow.getUpdateMenuRcv7Ctrl(), _this->isServer);
                uint8_t currSrvr = gui::mainWindow.getCurrServer();
                _this->CurrSrvr = currSrvr;

                if (_this->isARM)
                {
                    if (!gui::mainWindow.getUpdateMenuSnd7Ctrl(currSrvr))
                    {
                        if (_this->selectedListId != gui::mainWindow.getidOfList_ctrl(currSrvr))
                        {
                            flog::info("ARM gui::mainWindow.getUpdateMenuRcv7Ctrl()");
                            if (gui::mainWindow.getidOfList_ctrl(currSrvr) < _this->listNames.size())
                                _this->selectedListId = gui::mainWindow.getidOfList_ctrl(currSrvr);
                            else
                                _this->selectedListId = 0;
                            // gui::mainWindow.setMaxRecWaitTime_ctrl(MAX_SERVERS, _this->maxRecWaitTime);
                            gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrvr]);
                            // gui::mainWindow.setlevel_ctrl(currSrvr, _this->getgen_level[currSrvr]);
                            /// DMH _this->DelSelectedList();
                            config.acquire();
                            config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                            config.release(true);
                            _this->refreshWaterfallBookmarks(false);

                            _this->loadByName(_this->listNames[_this->selectedListId]);
                            /// DMH _this->AddSelectedList(); /// DMH
                            config.acquire();
                            config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                            config.conf["lists"][_this->selectedListName]["flaglevel"] = gui::mainWindow.getflag_level_ctrl(currSrvr);
                            config.conf["lists"][_this->selectedListName]["genlevel"] = gui::mainWindow.getLevelDbCtrl(currSrvr);
                            _this->getflag_level[currSrvr] = gui::mainWindow.getflag_level_ctrl(currSrvr);
                            // _this->flag_level = _this->getflag_level[currSrvr];
                            // _this->getgen_level[currSrvr] = gui::mainWindow.getlevel_ctrl(currSrvr);
                            // _this->genlevel = _this->getgen_level[currSrvr];

                            /*
                            if (config.conf["lists"][_this->selectedListName].contains("flaglevel")) {
                                _this->flag_level = config.conf["lists"][_this->selectedListName]["flaglevel"];
                            }
                            else {
                                config.conf["lists"][_this->selectedListName]["flaglevel"] = _this->flag_level;
                            }
                            if (config.conf["lists"][_this->selectedListName].contains("genlevel")) {
                                _this->flag_level = config.conf["lists"][_this->selectedListName]["genlevel"];
                            }
                            else {
                                config.conf["lists"][_this->selectedListName]["genlevel"] = _this->genlevel;
                            }
                            */
                            config.conf["selectedList"] = _this->selectedListName;
                            config.release(true);
                            _this->refreshWaterfallBookmarks(false);

                            /// DMH  core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);
                            _this->saveByName(_this->selectedListName);
                            _this->refreshLists();
                        }
                        if (_this->getgen_level[currSrvr] != gui::mainWindow.getLevelDbCtrl(currSrvr))
                        {
                            _this->getgen_level[currSrvr] = gui::mainWindow.getLevelDbCtrl(currSrvr);
                            config.acquire();
                            config.conf["genlevel"] = _this->getgen_level[currSrvr];
                            config.release(true);

                            flog::info("UPDATE _this->intLevel {0}", _this->getgen_level[currSrvr]);
                        }
                        if (_this->status_AKF != gui::mainWindow.getAKFInd_ctrl(currSrvr))
                        {
                            _this->status_AKF = gui::mainWindow.getAKFInd_ctrl(currSrvr);
                            config.acquire();
                            config.conf["status_AKF"] = _this->status_AKF;
                            config.release(true);
                            flog::info("UPDATE _this->status_AKF {0}", _this->status_AKF);
                        }
                        if (_this->status_auto_level4 != gui::mainWindow.getAuto_levelCtrl(currSrvr))
                        {
                            _this->status_auto_level4 = gui::mainWindow.getAuto_levelCtrl(currSrvr);
                            config.acquire();
                            config.conf["status_auto_level"] = _this->status_auto_level4;
                            config.release(true);
                            flog::info("UPDATE _this->status_auto_level {0}", _this->status_auto_level4);
                        }
                        gui::mainWindow.setFirstStart_ctrl(currSrvr, false);
                    }
                }

                if (_this->isServer && gui::mainWindow.getUpdateMenuRcv7Ctrl())
                {
                    _this->currSource = sourcemenu::getCurrSource();
                    if (!_first)
                        _first = gui::mainWindow.getFirstConn(NUM_MOD);
                    bool _update = false;

                    flog::info("gui::mainWindow.getUpdateMenuRcv7Ctrl()");
                    gui::mainWindow.setUpdateMenuRcv7Ctrl(false);
                    if (gui::mainWindow.getUpdateListRcv7Ctrl(currSrvr)) //  && !_this->running
                    {
                        std::lock_guard<std::mutex> lck(_this->scan3Mtx);
                        // std::lock_guard<std::mutex> lock(changeListsMtx);
                        flog::info("gui::mainWindow.getUpdateListRcv7Ctrl()");
                        gui::mainWindow.setUpdateListRcv7Ctrl(currSrvr, false);
                        config.acquire();
                        for (auto it = _this->listNames.begin(); it != _this->listNames.end(); ++it)
                        {
                            std::string name = *it;
                            if (name != "General")
                            {
                                // flog::info(" delete listName = {0}...", name);
                                config.conf["lists"].erase(name);
                            }
                        }
                        config.release(true);

                        // --- НАЧАЛО ИСПРАВЛЕННОГО БЛОКА ---

                        // Получаем данные и базово проверяем
                        int cnt_bbuf = gui::mainWindow.getsizeOfbbuf_ctrl();
                        if (cnt_bbuf <= 0)
                        {
                            flog::warn("getUpdateListRcv7Ctrl: Buffer from GUI is empty or invalid (size={0}).", cnt_bbuf);
                            continue; // Выходим, если данных нет
                        }
                        flog::info("gui::mainWindow.getUpdateListRcv7Ctrl() cnt_bbuf = {0}", cnt_bbuf);

                        // ИСПРАВЛЕНО: Проверяем, что размер буфера кратен размеру структуры, чтобы избежать сбоя
                        CtrlModeList sbm;
                        if (cnt_bbuf % sizeof(sbm) != 0)
                        {
                            flog::error("getUpdateListRcv7Ctrl: Buffer size ({0}) is not a multiple of structure size ({1}). Data is corrupted.", cnt_bbuf, sizeof(sbm));
                            continue; // Критическая ошибка, выходим
                        }

                        // ИСПРАВЛЕНО: Используем new[]/delete[] для идиоматичности
                        // uint8_t *bbufRCV = new uint8_t[cnt_bbuf];                         memcpy(bbufRCV, gui::mainWindow.getbbuf_ctrl(), cnt_bbuf);
                        auto bbufRCV = std::make_unique<uint8_t[]>(cnt_bbuf);
                        memcpy(bbufRCV.get(), gui::mainWindow.getbbuf_ctrl(), cnt_bbuf);

                        // Цикл по структурам в буфере
                        for (int poz = 0; poz < cnt_bbuf; poz += sizeof(sbm))
                        {
                            memcpy(&sbm, bbufRCV.get() + poz, sizeof(sbm));

                            // ИСПРАВЛЕНО: Безопасное создание строки, защита от отсутствия '\0'
                            std::string listname(sbm.listName, strnlen(sbm.listName, sizeof(sbm.listName)));

                            if (listname.empty())
                            {
                                flog::warn("Skipping a record with an empty listname at position {0}", poz);
                                continue;
                            }

                            json def = json::object();
                            def["bookmarks"] = json::object();

                            for (int i = 0; i < sbm.sizeOfList; i++)
                            {
                                std::string bookmarkname = "C" + std::to_string(sbm.bookmarkName[i]);
                                json &bookmark = def["bookmarks"][bookmarkname]; // Ссылка для удобства

                                bookmark["dopinfo"] = bookmarkname;

                                // --- ДОБАВЛЕНЫ ЯВНЫЕ ПРОВЕРКИ ---

                                // Проверка для frequency (предполагаем, что это float или double)
                                if (std::isfinite(sbm.frequency[i]))
                                {
                                    bookmark["frequency"] = sbm.frequency[i];
                                }
                                else
                                {
                                    bookmark["frequency"] = 0.0; // Безопасное значение по умолчанию
                                    continue;                    // Критическая ошибка, выходим
                                }

                                // Проверка для bandwidth
                                if (std::isfinite(sbm.bandwidth[i]))
                                {
                                    bookmark["bandwidth"] = sbm.bandwidth[i];
                                }
                                else
                                {
                                    bookmark["bandwidth"] = 12500.0; // Безопасное значение по умолчанию
                                }

                                // Проверка для level
                                if (std::isfinite(sbm.level[i]))
                                {
                                    bookmark["level"] = sbm.level[i];
                                }
                                else
                                {
                                    bookmark["level"] = -100.0; // Безопасное значение по умолчанию
                                }

                                bookmark["mode"] = sbm.mode[i];

                                std::string vsink = "vsink_" + std::to_string(_this->numInstance) + "_" + bookmarkname;
                                bookmark["scard"] = vsink;

                                // Проверка для Signal с дополнительной защитой try-catch
                                try
                                {
                                    if (std::isfinite(sbm.Signal[i]))
                                    {
                                        bookmark["Signal"] = sbm.Signal[i];
                                    }
                                    else
                                    {
                                        bookmark["Signal"] = 0; // Безопасное значение по умолчанию
                                    }
                                }
                                catch (const std::exception &e)
                                {
                                    bookmark["Signal"] = 0; // Безопасное значение при ошибке доступа
                                    std::cerr << "Exception while accessing Signal for " << bookmarkname << ": " << e.what() << '\n';
                                }
                            }

                            def["showOnWaterfall"] = true;
                            def["flaglevel"] = gui::mainWindow.getflag_level_ctrl(currSrvr);
                            def["genlevel"] = gui::mainWindow.getLevelDbCtrl(currSrvr);

                            _this->getflag_level[currSrvr] = gui::mainWindow.getflag_level_ctrl(currSrvr);
                            _this->getgen_level[currSrvr] = gui::mainWindow.getLevelDbCtrl(currSrvr);

                            // ИСПРАВЛЕНО: Блокировка только на время записи для повышения производительности
                            config.acquire();
                            try
                            {
                                config.conf["lists"][listname] = def;
                            }
                            catch (const std::exception &e)
                            {
                                flog::error("Failed to update config for list '{0}': {1}", listname, e.what());
                            }
                            config.release(true);
                        }
                        // delete[] bbufRCV;
                        // --- КОНЕЦ  БЛОКА ---

                        _this->refreshLists();
                        _this->refreshWaterfallBookmarks(false);

                        gui::mainWindow.setCTRLListNamesTxt(_this->listNamesTxt);
                        gui::mainWindow.setUpdateCTRLListForBotton(currSrvr, true);
                        // gui::mainWindow.setScanListNamesTxt(currSrvr, _this->listNamesTxt);
                        _update = true;
                    }
                    // _update = false;
                    if (_update || !gui::mainWindow.getUpdateMenuSnd7Ctrl(currSrvr))
                    {
                        _this->updateListAndState(_first, _update);
                    }
                    // flog::info("!!! 8 (end of list update block)");

                    if (gui::mainWindow.getUpdateModule_ctrl(currSrvr))
                    {
                        gui::mainWindow.setUpdateModule_ctrl(currSrvr, false);
                    }
                    if (_this->status_AKF != gui::mainWindow.getAKFInd_ctrl(currSrvr))
                    {
                        _this->status_AKF = gui::mainWindow.getAKFInd_ctrl(currSrvr);
                    }
                    if (_this->status_auto_level4 != gui::mainWindow.getAuto_levelCtrl(currSrvr))
                    {
                        _this->status_auto_level4 = gui::mainWindow.getAuto_levelCtrl(currSrvr);
                        config.acquire();
                        config.conf["status_auto_level"] = _this->status_auto_level4;
                        config.release(true);
                    }
                    if (gui::mainWindow.getLevelDbCtrl(currSrvr) != _this->getgen_level[currSrvr])
                    {
                        _this->getgen_level[currSrvr] = gui::mainWindow.getLevelDbCtrl(currSrvr);
                    }
                    if (gui::mainWindow.getMaxRecWaitTime_ctrl(currSrvr) != _this->maxRecWaitTime)
                    {
                        _this->maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_ctrl(currSrvr);
                        config.acquire();
                        config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
                        config.release(true);
                    }
                    _this->getflag_level[currSrvr] = gui::mainWindow.getflag_level_ctrl(currSrvr);
                    // _this->flag_level = _this->getflag_level[currSrvr];
                    // if (gui::mainWindow.getlevel_ctrl(currSrvr) != _this->genlevel) {
                    //    _this->genlevel = gui::mainWindow.getlevel_ctrl(currSrvr);
                    //    _this->getgen_level[currSrvr] = _this->genlevel;
                    //}

                    // }
                    // if (_this->currSource != SOURCE_ARM) {
                    // 1. Получаем желаемое состояние от клиента.
                    bool arm_run = gui::mainWindow.getbutton_ctrl(currSrvr);

                    // 2. Получаем реальное состояние модуля и определяем, активен ли он.
                    ModuleState actual_state = _this->currentState.load();
                    bool is_server_active = (actual_state == ModuleState::RUNNING || actual_state == ModuleState::STARTING);

                    flog::info("CTRL4 arm_run {0}, is_server_active {1}, actual_state {2}, currSrvr {3}", arm_run, is_server_active, (int)actual_state, currSrvr);
                    if (is_server_active != arm_run)
                    {
                        flog::info("CTRL4....");
                        if (arm_run)
                        { // Нужно запускать
                            flog::info("CTRL4 _this->start");
                            if (actual_state == ModuleState::STOPPED)
                            {
                                _this->start();
                            }
                            else
                            {
                                gui::mainWindow.setbutton_ctrl(0, false);
                                gui::mainWindow.setUpdateMenuSnd7Ctrl(0, true);
                                gui::mainWindow.setUpdateMenuSnd0Main(0, true);
                            }
                        }
                        else
                        { // Нужно останавливать
                            flog::info("CTRL4 _this->stop()");
                            gui::mainWindow.setServerRecordingStop(0);
                            gui::mainWindow.setRecording(false);
                            _this->stop();
                            _this->_recording = false;
                            _this->_detected = false;
                        }
                    }
                    flog::info("!!! 10");
                }
            }
        }
    }

    void getScanLists()
    {
        if (!isARM)
            return;
        flog::info("getScanLists started");

        // ИСПРАВЛЕНО: Используем unique_ptr для автоматического управления памятью (RAII).
        // Это полностью защищает от утечек памяти.
        auto bbuf = std::make_unique<uint8_t[]>(MAX_LIST_PACKET_CTRL_SIZE);
        int sizeofbbuf = 0;

        // ИСПРАВЛЕНО: Минимизируем время блокировки.
        // Копируем нужную часть JSON в локальную переменную и сразу отпускаем мьютекс.
        json lists_data;
        config.acquire();
        try
        {
            // Проверяем, существует ли вообще секция "lists"
            if (config.conf.contains("lists") && config.conf["lists"].is_object())
            {
                lists_data = config.conf["lists"];
            }
        }
        catch (const std::exception &e)
        {
            flog::error("Failed to access 'lists' in config: {0}", e.what());
        }
        config.release();

        if (lists_data.empty())
        {
            flog::warn("Config section 'lists' is missing or empty. No lists to scan.");
            // Отправляем пустой буфер, если нужно оповестить GUI об очистке
            gui::mainWindow.setbbuf_ctrl(bbuf.get(), 0);
            gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
            return;
        }

        CtrlModeList sbm;
        int list_count = 0;

        // Работаем с локальной копией `lists_data`, а не с `config.conf`
        for (auto &[listName, list] : lists_data.items())
        {
            // ИСПРАВЛЕНО: Используем snprintf для безопасного копирования строки с проверкой размера.
            snprintf(sbm.listName, sizeof(sbm.listName), "%s", listName.c_str());
            // Дополнительно убедимся, что строка завершается нулем, на случай усечения
            sbm.listName[sizeof(sbm.listName) - 1] = '\0';

            int bookmark_idx = 0;

            // ДОБАВЛЕНО: Проверяем, что в списке есть секция "bookmarks"
            if (!list.contains("bookmarks") || !list["bookmarks"].is_object())
            {
                continue; // Пропускаем список без закладок
            }

            for (auto &[bookmarkName, bm] : list["bookmarks"].items())
            {
                if (bookmarkName.empty() || bookmarkName[0] != 'C')
                {
                    flog::warn("Skipping invalid bookmark name '{0}' in list '{1}'", bookmarkName, listName);
                    continue;
                }
                try
                {
                    sbm.bookmarkName[bookmark_idx] = std::stoi(bookmarkName.substr(1));
                }
                catch (const std::exception &)
                {
                    flog::error("Could not parse number from bookmark '{0}'", bookmarkName);
                    continue; // Пропускаем некорректную закладку
                }

                // ДОБАВЛЕНО: Безопасная функция для извлечения числовых значений из JSON
                auto get_json_number = [&](const json &j_node, const std::string &key, double default_val)
                {
                    if (j_node.contains(key) && j_node[key].is_number())
                    {
                        return j_node[key].get<double>();
                    }
                    flog::warn("Field '{0}' in '{1}' is missing or not a number. Using default {2}.", key, bookmarkName, default_val);
                    return default_val;
                };

                sbm.frequency[bookmark_idx] = get_json_number(bm, "frequency", 0.0);
                sbm.bandwidth[bookmark_idx] = get_json_number(bm, "bandwidth", 12500.0);
                sbm.mode[bookmark_idx] = static_cast<int>(get_json_number(bm, "mode", 0));
                sbm.Signal[bookmark_idx] = get_json_number(bm, "Signal", 0.0);
                sbm.level[bookmark_idx] = get_json_number(bm, "level", -50.0);

                bookmark_idx++;
                if (bookmark_idx >= MAX_COUNT_OF_CTRL_LIST)
                {
                    flog::warn("Max number of bookmarks ({0}) reached for list '{1}'", MAX_COUNT_OF_CTRL_LIST, listName);
                    break;
                }
            }

            sbm.sizeOfList = bookmark_idx;

            // ИСПРАВЛЕНО: Проверка на переполнение основного буфера `bbuf`
            if (sizeofbbuf + sizeof(sbm) > MAX_LIST_PACKET_CTRL_SIZE)
            {
                flog::error("Not enough space in control buffer to add list '{0}'. Stopping.", listName);
                break;
            }

            // Копируем готовую структуру в буфер. Используем .get() для доступа к сырому указателю.
            memcpy(bbuf.get() + sizeofbbuf, &sbm, sizeof(sbm));
            sizeofbbuf += sizeof(sbm);

            flog::info("{0}. listName='{1}'. Bookmarks: {2}. Total buffer size: {3}", list_count, listName, sbm.sizeOfList, sizeofbbuf);
            list_count++;
            if (list_count >= MAX_BM_SIZE)
            {
                flog::warn("Max number of lists ({0}) reached.", MAX_BM_SIZE);
                break;
            }
        }

        flog::info("getScanLists finished. Total lists: {0}, total buffer size: {1}", list_count, sizeofbbuf);

        // Передаем указатель на данные в GUI. GUI ДОЛЖЕН скопировать эти данные, а не хранить указатель.
        gui::mainWindow.setbbuf_ctrl(bbuf.get(), sizeofbbuf);
        // ::operator delete(bbuf); <-- БОЛЬШЕ НЕ НУЖНО! Память освободится автоматически.
        /*
        CurrSrvr = gui::mainWindow.getCurrServer();
        gui::mainWindow.setidOfList_ctrl(CurrSrvr, selectedListId);
        gui::mainWindow.setflag_level_ctrl(CurrSrvr, getflag_level[CurrSrvr]);
        gui::mainWindow.setLevelDbCtrl(CurrSrvr, getgen_level[CurrSrvr]);
        gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
        gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
        */
        flog::warn("CTRL msgCtrl ({0}) sizeofbbuf: {1}, gui::mainWindow.getUpdateMenuSnd7Ctrl {2}, flag_level {3}", CurrSrvr, sizeofbbuf, gui::mainWindow.getUpdateMenuSnd7Ctrl(CurrSrvr), getflag_level[CurrSrvr]);
    }

    std::string exec(const char *cmd)
    {
        char buffer[1280];
        std::string result = "";
        FILE *pipe = popen(cmd, "r");
        // flog::info("exec(.) cmd={0}", cmd);
        if (!pipe)
            throw std::runtime_error("popen() failed!");
        try
        {
            while (fgets(buffer, sizeof buffer, pipe) != NULL)
            {
                result += buffer;
            }
        }
        catch (...)
        {
            pclose(pipe);
            throw;
        }
        pclose(pipe);
        // flog::info("exec(..) result =  {1}\n", cmd, result);
        return result;
    }

    bool controlVirtCards(std::string vsink)
    {
        bool _add = false;
        // std::string vcard = "pacmd list-sources | grep '" + vsink + "'";
        std::string vcard = "pactl list short modules | grep '" + vsink + "'";
        std::string result = exec(vcard.c_str());
        // flog::info("controlVirtCards {0}, cmd: {1}, result ={2}\n", vsink, vcard, result);
        if (result == "")
        {
            // vcard = "pactl load-module module-virtual-sink sink_name='" + vsink +"'";
            vcard = "pactl load-module module-virtual-sink sink_name=" + vsink;
            exec(vcard.c_str());
            // flog::info("Додати....{0}, cmd: {1}, result ={2}", vsink, vcard, result);
            _add = true;
        }
        else
        {
            // flog::info("Vcard is exist!");
            _add = false;
        }
        return _add;
    }

    bool unloadVirtCards(std::string vsink)
    {
        bool _add = false;
        // pactl list short modules
        // pactl unload-module
        // pulseaudio -k              //  clear all -
        std::string vcard = "pactl list short modules | grep '" + vsink + "'";
        std::string result = exec(vcard.c_str());
        // flog::info("unloadVirtCards....{0}, cmd: {1}, result ={2}", bookmarks.size(), vcard, result);
        // flog::info("Додати....{0}, cmd: {1}, result ={2}", bookmarks.size(), vcard, result);
        if (result != "")
        {
            std::string num = "";
            for (int i = 0; i < result.size(); i++)
            {
                // flog::info("
                if (result[i] < 48)
                    break;
                num = num + result[i];
            }
            // flog::info("Vcard is exist! num = {0}", num);
            std::string unload_cmd = "pactl unload-module " + num;
            exec(unload_cmd.c_str());
            // flog::info("unloadVirtCards....{0}, cmd: {1}, result ={2}\n", bookmarks.size(), vcard, result);
        }
        return _add;
    }

    static void applyMode(ObservationBookmark bm, std::string vfoName)
    {
        if (vfoName != "")
        {
            if (core::modComManager.interfaceExists(vfoName))
            {
                if (core::modComManager.getModuleName(vfoName) == "radio")
                {
                    int mode = bm.mode;
                    double bandwidth = (double)bm.bandwidth;
                    flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 659");
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            tuner::normalTuning(gui::waterfall.selectedVFO, bm.frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
        else
        {
            gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
            gui::waterfall.setCenterFrequency(bm.frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
    }

    static void applyBookmark(ObservationBookmark bm, std::string vfoName)
    {
        if (vfoName == "")
        {
            // TODO: Replace with proper tune call
            gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
            gui::waterfall.setCenterFrequency(bm.frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
        else
        {
            if (core::modComManager.interfaceExists(vfoName))
            {
                if (core::modComManager.getModuleName(vfoName) == "radio")
                {
                    int mode = bm.mode;
                    double bandwidth = (double)bm.bandwidth;
                    // flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 685");
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }
    }

    struct setDev
    {
        std::string sndname;
        int id;
    } psetDev;

    // Проверка, является ли Unicode-символ допустимым (буква, цифра, пробел)
    bool isAllowedUnicodeChar(uint32_t cp)
    {
        return (cp >= '0' && cp <= '9') ||                             // цифры
               (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || // латиница
               (cp >= 0x0400 && cp <= 0x04FF) ||                       // кириллица
               (cp == ' ') || (cp == '-') || (cp == '_');              // пробел, тире, подчёркивание
    }

    // Простой UTF-8 парсер: очищает строку от "спецсимволов"
    std::string removeSpecialChars(const std::string &input)
    {
        std::string result;
        size_t i = 0;

        while (i < input.size())
        {
            uint32_t cp = 0;
            unsigned char c = input[i];

            if (c < 0x80)
            { // ASCII (1 байт)
                cp = c;
                ++i;
            }
            else if ((c >> 5) == 0x6)
            { // 110xxxxx 10xxxxxx
                if (i + 1 >= input.size())
                    break;
                cp = ((c & 0x1F) << 6) | (input[i + 1] & 0x3F);
                i += 2;
            }
            else if ((c >> 4) == 0xE)
            { // 1110xxxx 10xxxxxx 10xxxxxx
                if (i + 2 >= input.size())
                    break;
                cp = ((c & 0x0F) << 12) | ((input[i + 1] & 0x3F) << 6) | (input[i + 2] & 0x3F);
                i += 3;
            }
            else if ((c >> 3) == 0x1E)
            { // 11110xxx ...
                // слишком редкий случай, скипаем
                i += 4;
                continue;
            }
            else
            {
                // недопустимая последовательность
                ++i;
                continue;
            }

            if (isAllowedUnicodeChar(cp))
            {
                // перекодируем обратно в UTF-8
                if (cp < 0x80)
                {
                    result += static_cast<char>(cp);
                }
                else if (cp < 0x800)
                {
                    result += static_cast<char>((cp >> 6) | 0xC0);
                    result += static_cast<char>((cp & 0x3F) | 0x80);
                }
                else
                {
                    result += static_cast<char>((cp >> 12) | 0xE0);
                    result += static_cast<char>(((cp >> 6) & 0x3F) | 0x80);
                    result += static_cast<char>((cp & 0x3F) | 0x80);
                }
            }
        }

        return result;
    }

    std::string makeUniqueName(const std::string &rawName, const std::vector<std::string> &listNames)
    {
        // flog::info("rawName {0}", rawName);
        auto toLower = [](const std::string &str) -> std::string
        {
            std::string lowerStr;
            lowerStr.reserve(str.size());
            for (char c : str)
            {
                lowerStr += std::tolower(static_cast<unsigned char>(c));
            }
            return lowerStr;
        };

        std::string baseName = removeSpecialChars(rawName);
        std::string uniqueName = baseName;
        int counter = 1;

        auto nameExists = [&](const std::string &name)
        {
            std::string lowerName = toLower(name);
            return std::any_of(listNames.begin(), listNames.end(), [&](const std::string &existing)
                               { return toLower(existing) == lowerName; });
        };

        while (nameExists(uniqueName))
        {
            uniqueName = baseName + "(" + std::to_string(counter++) + ")";
        }
        // flog::info("uniqueName {0}", uniqueName);
        return uniqueName;
    }

    bool bookmarkEditDialog()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        std::string vsink = "";
        if (!editOpen)
        {
            vsink = "vsink_" + std::to_string(numInstance) + "_" + editedBookmarkName;
            editedBookmark.scard = vsink;
        }
        else
        {
            vsink = editedBookmark.scard;
        }

        std::string id = "Edit " + editedBookmarkName + "##supervisor_edit_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        // applyBookmark(editedBookmark, gui::waterfall.selectedVFO);

        // strcpy(nameBuf, editedBookmarkName.c_str());
        char nameBuf[32];
        strcpy(nameBuf, dop_info.c_str());

        char nameSink[128];
        strcpy(nameSink, vsink.c_str());
        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::BeginTable(("supervisor_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Назва");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            if (ImGui::InputText(("##supervisor_edit_name_4" + name).c_str(), nameBuf, 32))
            {
                // editedBookmarkName = nameBuf;
                dop_info = std::string(nameBuf);
                //               flog::info("!!! editedBookmarkName={0}, nameBuf={1}", editedBookmarkName, nameBuf);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Частота, кГц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);

            if (ImGui::InputDouble(("##supervisor_edit_freq_4" + name).c_str(), &_frec, 1, 100, "%.0f"))
            {
                _frec = std::clamp<double>(_frec, 10000.0, 1999999.0);
            };
            ImGui::SameLine();
            if (ImGui::Button(".000"))
            { // 0xF7 247
                _frec = (double)round(_frec);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (radioMode == 2)
            { // MODE_AZALIY
                ImGui::LeftLabel("Смуга, Гц");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(250);
                if (ImGui::InputFloat(("##fsupervisor_edit_bw_4" + name).c_str(), &editedBookmark.bandwidth, 100, 100, "%.0f"))
                {
                    // editedBookmark.bandwidth = std::clamp<float>(editedBookmark.bandwidth, minBandwidth, maxBandwidth);
                    // setBandwidth(_this->bandwidth);
                }
            }
            else
            {
                /*
                ImGui::LeftLabel("Смуга, кГц");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(250);
                // ImGui::InputDouble(("##supervisor_edit_bw_4" + name).c_str(), &editedBookmark.bandwidth);
                if (ImGui::Combo(("##fsupervisor_edit_bw_4" + name).c_str(), &_bandwidthId, bandListTxt))
                {
                    editedBookmark.bandwidth = bandwidthsList[_bandwidthId];
                    flog::info("TRACE. editedBookmark.bandwidth = {0} !", editedBookmark.bandwidth);
                } */
                float kHzbandwidth = 1.0;
                if (editedBookmark.bandwidth < 1000)
                    kHzbandwidth = 1.0;
                else
                    kHzbandwidth = editedBookmark.bandwidth / 1000.0;
                ImGui::LeftLabel("Смуга, кГц");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(250);

                if (ImGui::InputFloat(("##_radio_bw_ctrl4_" + name).c_str(), &kHzbandwidth, 1.25, 10.0, "%.2f"))
                {
                    int max_bw = bandwidthsList[bandwidthsList.size() - 1];
                    // if (max_bw > _this->maxBandwidth)
                    //     max_bw = _this->maxBandwidth;
                    float bw = std::clamp<double>(kHzbandwidth * 1000, 1, max_bw);
                    if (bw >= editedBookmark.bandwidth)
                    {
                        for (int i = 1; i < bandwidthsList.size(); i++)
                        {
                            if (bw < bandwidthsList[i])
                            {
                                bw = bandwidthsList[i];
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (int i = bandwidthsList.size() - 1; i >= 0; i--)
                        {
                            if (bw > bandwidthsList[i])
                            {
                                bw = bandwidthsList[i];
                                break;
                            }
                        }
                    }
                    if (bw < 1000)
                    {
                        bw = 1000;
                    }
                    editedBookmark.bandwidth = bw;
                }
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Вид демод.");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);

            // ImGui::Combo(("##supervisor_edit_mode_4" + name).c_str(), &editedBookmark.mode, demodModeListTxt);
            if (ImGui::Combo(("##supervisor_edit_mode_4" + name).c_str(), &editedBookmark.mode, demodModeListTxt, 9))
            {
                editedBookmark.mode = editedBookmark.mode;
                if (editedBookmark.mode == 7)
                {
                    _raw = true;
                    editedBookmark.bandwidth = 220000;
                    _bandwidthId = 6;
                }
                else
                {
                    _raw = false;
                }

                flog::info("TRACE. editedBookmark.mode = {0} !", editedBookmark.mode);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Тип сигналу");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);

            if (ImGui::Combo(("##supervisor_edit_signal_4" + name).c_str(), &editedBookmark.Signal, SignalListTxt))
            {
                editedBookmark.Signal = editedBookmark.Signal;
                if (editedBookmark.Signal < 0 || editedBookmark.Signal > 2)
                {
                    editedBookmark.Signal = 0;
                }

                flog::info("TRACE. editedBookmark.mode = {0} !", editedBookmark.mode);
            }
            /*
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Поріг, дБ");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            ImGui::InputInt(("##supervisor_edit_level_4" + name).c_str(), &editedBookmark.level, -150, 0);
            */
            editedBookmark.level = -70; // std::clamp<int>(editedBookmark.level, -150, -30);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Звукова карта");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            style::beginDisabled();
            if (ImGui::InputText(("##supervisor_dev_4" + name).c_str(), nameSink, 128))
            {
                // vsink = "vsink" + std::to_string(bank_size);
                editedBookmark.scard = std::string(vsink);
                flog::info("!!! editedBookmarkName={0}, nameBuf={1}", editedBookmarkName, nameSink);
            }
            style::endDisabled();
            /*
            if (ImGui::Combo(("##supervisor_dev_4" + name).c_str(), &devListId, currDevList.c_str())) {
                editedBookmark.scard = currVCardDevList[devListId];
                flog::info("!!! devListId={0}, currVCardDevList[devListId]={1}", devListId, currVCardDevList[devListId]);
            }
            */
            ImGui::EndTable();

            //  bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName) || (currDevList=="");
            int type_error = 0;
            bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);
            // Ввести обмеження по діапазону 25.....1700 МГц (для сканування.спостереження)

            if (editedBookmark.frequency < MIN_FREQ)
            {
                applyDisabled == false;
                type_error = 3;
            }
            if (editedBookmark.frequency > MAX_FREQ)
            {
                applyDisabled == false;
                type_error = 4;
            }

            if (applyDisabled == false)
            {
                editedBookmark.frequency = round(_frec * 1000);
                double min_frec = editedBookmark.frequency;
                double max_frec = editedBookmark.frequency;
                if (editOpen == false)
                {
                    for (auto &[name, bm] : bookmarks)
                    {
                        // flog::warn("Додати.... bm.frequency {0}, editedBookmark.frequency = {1} (== oldFreq)", bm.frequency, editedBookmark.frequency);
                        if (bm.frequency == editedBookmark.frequency && bm.mode == editedBookmark.mode)
                        {
                            applyDisabled = true;
                            type_error = 1;
                            // flog::warn("Додати....{0}.editedBookmark.frequency = {1} (== oldFreq)", bookmarks.size(), editedBookmark.frequency);
                            break;
                        }
                        // flog::warn("bm.frequency={0}, min_frec {1}, max_frec {2}", bm.frequency, min_frec, max_frec);
                        if (bm.frequency > max_frec)
                            max_frec = bm.frequency;
                        if (bm.frequency < min_frec)
                            min_frec = bm.frequency;
                    }
                }
                else
                {
                    int cnt = 0;
                    min_frec = round(_frec * 1000);
                    max_frec = round(_frec * 1000);
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (bm.frequency == editedBookmark.frequency && NumIDBookmark != cnt)
                        {
                            flog::info("bm.frequency {0} == editedBookmark.frequency {1}, cnt {2}", bm.frequency, editedBookmark.frequency, cnt);
                            applyDisabled = true;
                            type_error = 1;
                            continue;
                        }
                        cnt++;

                        if (bm.frequency == editedBookmark.frequency)
                            continue;
                        if (bm.frequency > max_frec)
                            max_frec = bm.frequency;
                        if (bm.frequency < min_frec)
                            min_frec = bm.frequency;
                    }
                }
                if (applyDisabled == false)
                {
                    double difference = max_frec - min_frec;
                    if (difference > CMO)
                    {
                        type_error = 2;
                        applyDisabled = true;
                        // flog::warn("Додати... {0}.editedBookmark.frequency = {1};  max_frec: {2}, min_frec ={3}, difference = {4}, ", bookmarks.size(), editedBookmark.frequency, max_frec, min_frec, difference);
                    }
                }
            }

            if (applyDisabled)
            {
                style::beginDisabled();
            }
            if (ImGui::Button("   OK   "))
            { /// Apply
                if (controlVirtCards(vsink))
                {
                    // core::modComManager.callInterface("Audio", AUDIO_SINK_GET_DEVLIST, NULL, &txtDevList);
                }

                editedBookmark.dopinfo = dop_info;

                open = false;
                editedBookmark.frequency = round(_frec * 1000); // _frec;
                if (_raw == true)
                {
                    editedBookmark.bandwidth = sigpath::iqFrontEnd.getSampleRate();
                }

                if (!editOpen)
                {
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (name == editedBookmarkName)
                        {
                            flog::error("!!! ERROR add new freq  {0}", editedBookmarkName);
                            return false;
                        }
                    }
                }
                flog::info("!!! 2 editedBookmarkName={0}, editedBookmark.scard={1}, firstEditedBookmarkName = {2}, editOpen={3}", editedBookmarkName, editedBookmark.scard, firstEditedBookmarkName, editOpen);
                if (editOpen && !isARM)
                {
                    bool _del = false;
                    if (!isARM)
                    {
                        core::moduleManager.deleteInstance(firstEditedBookmarkName);
                        std::string rec_name = "Запис " + firstEditedBookmarkName;
                        core::moduleManager.deleteInstance(rec_name);
                    }
                    _del = true;
                    if (_del == true)
                        bookmarks.erase(firstEditedBookmarkName);
                    else
                    {
                        flog::error("!!! ERROR delete Instance {0}", firstEditedBookmarkName);
                        return false;
                    }
                }

                ObservationBookmark &bm = editedBookmark;
                double _frequency = editedBookmark.frequency;
                double _offset = sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                double _central = gui::waterfall.getCenterFrequency();

                std::string str = editedBookmarkName;
                char modName[1024];
                char recmodName[1024];

                strcpy(modName, (char *)str.c_str());
                bool modified = false;
                flog::info("!!! modName={0}, editedBookmark.frequency={1}", modName, str.c_str());
                // Add Radio
                if (!isARM)
                {
                    if (!core::moduleManager.createInstance(modName, "radio"))
                    {
                        // flog::info("!!! 1 modName {0}", modName);
                        core::moduleManager.postInit(modName);
                        int mode = (int)editedBookmark.mode;
                        double bandwidth = (double)editedBookmark.bandwidth;
                        flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 920");
                        core::modComManager.callInterface(modName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                        core::modComManager.callInterface(modName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                        modified = true;
                        flog::info("!!! Канал приймання successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", modName, editedBookmarkName, editedBookmark.frequency);
                    }
                    else
                    {
                        core::moduleManager.deleteInstance(modName);
                        if (!core::moduleManager.createInstance(modName, "recorder"))
                        {
                            flog::info("!!! 2");
                            core::moduleManager.postInit(modName);
                            flog::info("!!! 2");
                            modified = true;
                            flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", modName, editedBookmarkName, editedBookmark.frequency);
                        }
                        else
                        {
                            modified = false;
                            modified = true;
                            flog::error("!!! ERROR adding radio. modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", modName, editedBookmarkName, editedBookmark.frequency);
                        }
                    }

                    // Add Record
                    std::string recName = "Запис " + editedBookmarkName;
                    strcpy(recmodName, (char *)recName.c_str());
                    if (!core::moduleManager.createInstance(recmodName, "recorder"))
                    {
                        flog::info("!!! 3");
                        core::moduleManager.postInit(recmodName);
                        flog::info("!!! 3");
                        core::modComManager.callInterface(recName, RECORDER_IFACE_CMD_SET_STREAM, (void *)editedBookmarkName.c_str(), NULL);
                        modified = true;
                        flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", recmodName, editedBookmarkName, editedBookmark.frequency);
                    }
                    else
                    {
                        if (modified == true)
                        {
                            core::moduleManager.deleteInstance(recmodName);
                            if (!core::moduleManager.createInstance(recmodName, "recorder"))
                            {
                                flog::info("!!! 3");
                                core::moduleManager.postInit(recmodName);
                                flog::info("!!! 4");
                                modified = true;
                                flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", recmodName, editedBookmarkName, editedBookmark.frequency);
                            }
                            else
                            {
                                modified = false;
                                flog::error("!!! ERROR adding Recorder! modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", recmodName, editedBookmarkName, editedBookmark.frequency);
                            }
                        }
                        else
                        {
                            modified = false;
                            flog::error("!!! ERROR adding Recorder! modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", recmodName, editedBookmarkName, editedBookmark.frequency);
                        }
                    }
                }
                else
                {
                    modified = true;
                }
                flog::info(" TRACE centerFrequency = {0}, editedBookmark.frequency = {1}, gui::waterfall.selectedVFO = {2} ", gui::waterfall.getCenterFrequency(), _frequency, gui::waterfall.selectedVFO);

                if (modified)
                {
                    // If editing, delete the original one
                    // recName
                    std::string recName = "Запис " + editedBookmarkName;
                    if (!isARM)
                    {
                        std::string dev;
                        std::stringstream ss(txtDevList);
                        int i = 0;
                        while (getline(ss, dev, '\0'))
                        {
                            flog::info("{0}, vsink ={1}, dev = {2}", i, vsink, dev);
                            if (dev.find(vsink) < dev.length())
                            {
                                break;
                            }
                            i++;
                        }
                        devListId = 0; // i;
                        flog::info(" TRACE modName = {0}, devListId = {1}", modName, devListId);
                        psetDev.id = devListId;
                        psetDev.sndname = modName;
                        // core::modComManager.callInterface("Audio", AUDIO_SINK_CMD_SET_DEV, (void*)&psetDev, NULL);
                    }
                    if (editOpen)
                    {
                        bookmarks.erase(firstEditedBookmarkName);
                    }
                    bookmarks[editedBookmarkName] = editedBookmark;

                    saveByName(selectedListName);

                    // Update enabled and disabled modules
                    core::configManager.acquire();
                    json instances;
                    for (auto [_name, inst] : core::moduleManager.instances)
                    {
                        instances[_name]["module"] = inst.module.info->name;
                        instances[_name]["enabled"] = inst.instance->isEnabled();
                    }
                    core::configManager.conf["moduleInstances"] = instances;
                    core::configManager.release(true);
                    if (!isARM)
                    {
                        gui::waterfall.setCurrVFO(editedBookmarkName);

                        gui::waterfall.selectedVFO = editedBookmarkName;

                        // usleep(200);

                        tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, bm.frequency);

                        applyMode(bm, gui::waterfall.selectedVFO);

                        int mode = bm.mode;
                        double bandwidth = (double)bm.bandwidth;
                        flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 1027");
                        core::modComManager.callInterface(editedBookmarkName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);

                        core::modComManager.callInterface(editedBookmarkName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);

                        core::modComManager.callInterface(recName, RECORDER_IFACE_CMD_SET_STREAM, (void *)editedBookmarkName.c_str(), NULL);

                        gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());

                        gui::waterfall.setCenterFrequency(_central);

                        sigpath::vfoManager.setOffset(gui::waterfall.selectedVFO, _offset);
                        // usleep(100);

                        // gui::waterfall.centerFreqMoved = true;

                        // gui::waterfall.selectedVFO = "Канал приймання";
                        gui::waterfall.setCurrVFO("Канал приймання");
                    }
                    flog::info(" 5 TRACE gui::waterfall.selectedVFO = {0}", gui::waterfall.selectedVFO);

                    // flog::info(" TRACE centerFrequency = {0}, editedBookmark.frequency = {1}, _offset = {2}, gui::waterfall.selectedVFO = {3}, stream ={4} ", gui::waterfall.getCenterFrequency(), _frequency, _offset, gui::waterfall.selectedVFO, stream);
                    bool _fake = true;
                    // flog::info("DelSelectedList->");
                    /// DMH DelSelectedList(_fake);
                    config.acquire();
                    config.conf["lists"][selectedListName]["showOnWaterfall"] = false;
                    config.release(true);
                    refreshWaterfallBookmarks(false);
                    ///  flog::info("AddSelectedList->");
                    /// DMH AddSelectedList(_fake);
                    loadByName(listNames[selectedListId]);
                    // flog::info("111 flag_level {0}", flag_level);
                    config.acquire();
                    config.conf["selectedList"] = selectedListName;
                    config.conf["lists"][selectedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][selectedListName]["flaglevel"] = getflag_level[CurrSrvr];
                    config.conf["lists"][selectedListName]["genlevel"] = getgen_level[CurrSrvr];
                    config.release(true);

                    refreshLists();
                    gui::mainWindow.setCTRLListNamesTxt(listNamesTxt);
                    gui::mainWindow.setUpdateModule_ctrl(CurrSrvr, true);
                    gui::mainWindow.setUpdateCTRLListForBotton(CurrSrvr, true);

                    getScanLists();
                    gui::mainWindow.setidOfList_ctrl(CurrSrvr, selectedListId);
                    gui::mainWindow.setflag_level_ctrl(CurrSrvr, getflag_level[CurrSrvr]);
                    gui::mainWindow.setLevelDbCtrl(CurrSrvr, getgen_level[CurrSrvr]);
                    gui::mainWindow.setAKFInd_ctrl(MAX_SERVERS, status_AKF);
                    gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);

                    refreshWaterfallBookmarks(false);
                }
            }
            if (applyDisabled)
            {
                style::endDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати"))
            {
                open = false;
            }

            if (type_error > 0)
            {
                std::string Str = "";
                int this_freq = round(editedBookmark.frequency);
                if (type_error == 1)
                    Str = "Значення частоти (" + std::to_string(this_freq) + ") вже присутнє y поточному банку!";
                else if (type_error == 2)
                    Str = "Налаштування повинні бути в межах CMO (" + std::to_string(CMO) + ")!";
                else if (type_error == 3)
                    Str = "Значення частоти (" + std::to_string(this_freq) + ") меньше обмеженного " + std::to_string(MIN_FREQ) + "!";
                else if (type_error == 4)
                    Str = "Значення частоти (" + std::to_string(this_freq) + ") більше обмеженного " + std::to_string(MAX_FREQ) + "!";

                ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", Str.c_str());
            }

            ImGui::EndPopup();
        }
        bookmarks_size = bookmarks.size();
        return open;
    }

    void restoreListSelections_forceRemap(const std::map<int, std::string> &oldSelectedNames, const std::vector<std::string> &listNames, std::string &listNamesTxt)
    {
        for (int i = 0; i < MAX_SERVERS; ++i)
        {
            if (i == gui::mainWindow.getCurrServer())
                continue;

            auto it = oldSelectedNames.find(i);
            if (it == oldSelectedNames.end())
                continue;

            const std::string &name = it->second;

            // Найти НОВЫЙ НОМЕР по ИМЕНИ
            int newId = 0;
            auto found = std::find(listNames.begin(), listNames.end(), name);
            if (found != listNames.end())
            {
                newId = static_cast<int>(std::distance(listNames.begin(), found));
            }
            else if (!listNames.empty())
            {
                newId = static_cast<int>(listNames.size()) - 1;
            }

            // ВСЕГДА ПЕРЕУСТАНАВЛИВАЕМ (даже если номер совпадает)

            gui::mainWindow.setidOfList_ctrl(i, newId);
            // gui::mainWindow.setUpdateCTRLListForBotton(i, true);

            // gui::mainWindow.setUpdateModule_ctrl(i, newId);
        }
    }

    bool newListDialog()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##supervisor_new_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[32];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::LeftLabel("Назва банку ");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##supervisor_edit_name_4" + name).c_str(), nameBuf, 32))
            {
                editedListName = makeUniqueName(std::string(nameBuf), listNames);
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists)
            {
                style::beginDisabled();
            }
            if (ImGui::Button("   OK   ") && listNames.size() < MAX_BM_SIZE)
            {
                listUpdater.store(true);
                open = false;
                std::vector<std::string> listNamesCopy = listNames;
                std::map<int, std::string> oldSelectedNames;
                for (int i = 0; i < MAX_SERVERS; ++i)
                {
                    int listId = gui::mainWindow.getidOfList_ctrl(i);
                    if (listId >= 0 && listId < static_cast<int>(listNamesCopy.size()))
                    {
                        oldSelectedNames[i] = listNamesCopy[listId];
                    }
                }

                config.acquire();
                if (renameListOpen)
                {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else
                {
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                config.conf["lists"][editedListName]["flaglevel"] = false;
                config.conf["lists"][editedListName]["genlevel"] = -70;
                config.release(true);
                refreshLists();
                loadByName(editedListName);

                getScanLists();
                gui::mainWindow.setflag_level_ctrl(CurrSrvr, getflag_level[CurrSrvr]);
                gui::mainWindow.setLevelDbCtrl(CurrSrvr, getgen_level[CurrSrvr]);
                gui::mainWindow.setAKFInd_ctrl(CurrSrvr, status_AKF);
                gui::mainWindow.setCTRLListNamesTxt(listNamesTxt);
                gui::mainWindow.setUpdateModule_ctrl(CurrSrvr, true);
                gui::mainWindow.setidOfList_ctrl(CurrSrvr, selectedListId);
                restoreListSelections_forceRemap(oldSelectedNames, listNames, listNamesTxt);
                gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
                gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
                listUpdater.store(false);

                // gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);

                // curr_listName = editedListName;
            }
            if (strlen(nameBuf) == 0 || alreadyExists)
            {
                style::endDisabled();
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

    bool selectListsDialog()
    {
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "Вибір Банку##supervisor_sel_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items())
            {
                bool shown = true; // list["showOnWaterfall"];
                if (ImGui::Checkbox((listName + "##supervisor_sel_list_4").c_str(), &shown))
                {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    config.release(true);
                }
            }

            if (ImGui::Button("Ok"))
            {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    void refreshLists()
    {
        listNames.clear();
        listNamesTxt = "";

        config.acquire();
        for (auto [key, list] : config.conf["lists"].items())
        {
            std::string _name = key; //  makeUniqueName(key, listNames);
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void refreshWaterfallBookmarks(const bool modif_vfo = false) override
    {
        waterfallBookmarks.clear();
        // flog::info("[CONTROL] refreshWaterfallBookmarks. modif_vfo ={0}, config.conf[] {1}", modif_vfo, config.conf["lists"].size());
        config.acquire();
        if (modif_vfo)
            core::configManager.acquire();
        if (config.conf.contains("lists") && config.conf["lists"].contains(selectedListName))
        {
            // flog::info("[CONTROL]. Processing list: {0}", selectedListName);
            auto &list = config.conf["lists"][selectedListName];
            if (!list.contains("bookmarks"))
            {
                flog::warn("List '{0}' does not contain 'bookmarks'.", selectedListName);
                return;
            }
            auto &bookmarks = list["bookmarks"];

            if (!bookmarks.is_object())
            {
                flog::warn("'bookmarks' section in list '{0}' is not an object. Skipping.", selectedListName);
                if (modif_vfo)
                    core::configManager.release(true);
                config.release();
                return;
            }

            for (auto &[bookmarkName, bm] : bookmarks.items())
            {
                WaterfallBookmark wbm;

                try
                {
                    wbm.listName = selectedListName; // Имя списка всегда одно и то же
                    wbm.bookmarkName = bookmarkName;

                    // 4. Безопасно извлекаем данные, используя .value() с дефолтными значениями
                    // Это заменяет все твои прямые доступы и try-catch блоки
                    wbm.bookmark.frequency = bm.value("frequency", 0.0);
                    wbm.bookmark.bandwidth = bm.value("bandwidth", 10000.0);
                    wbm.bookmark.mode = bm.value("mode", 0);
                    wbm.bookmark.scard = bm.value("scard", "");
                    wbm.bookmark.Signal = bm.value("Signal", 0);
                    wbm.bookmark.level = bm.value("level", -70);
                    wbm.bookmark.dopinfo = bm.value("dopinfo", "");

                    /*
                    std::string vsink = wbm.bookmark.scard;
                    if (!isARM)
                    {
                        if (controlVirtCards(vsink))
                            // core::modComManager.callInterface("Audio", AUDIO_SINK_GET_DEVLIST, NULL, &txtDevList);
                    }
                    */
                    wbm.bookmark.selected = false;
                    wbm.notValidAfter = 0;
                    wbm.extraInfo = "";
                    wbm.worked = false;
                    waterfallBookmarks.push_back(wbm);
                }
                catch (const nlohmann::json::exception &e)
                {
                    // Если произошла ошибка (например, неправильный тип), логируем и продолжаем
                    flog::error("Failed to parse bookmark '{0}' in list '{1}'. Reason: {2}",
                                bookmarkName, selectedListName, e.what());
                    // Продолжаем цикл со следующей закладкой
                    continue;
                }

                if (!isARM && modif_vfo)
                {
                    double mainFreq = CentralFreq;
                    if (mainFreq <= 0)
                    {
                        mainFreq = gui::waterfall.getCenterFrequency();
                    }
                    // double mainFreq = gui::waterfall.getCenterFrequency();
                    double CurrFreq = wbm.bookmark.frequency;
                    double generalOffset = CurrFreq - mainFreq;
                    flog::warn("[CONTROL] CurrFreq {0} - mainFreq {1} =  {2}", CurrFreq, mainFreq, generalOffset);

                    if (std::abs(generalOffset) > VIEWBANDWICH / 2) // 4250000.0
                    {
                        flog::error("[CONTROL] generalOffset {0} за пределами допустимого диапазона ±425000 Гц. Устанавливается 0", generalOffset);
                        generalOffset = 0.0;
                    }
                    // ./dflog::warn("[CONTROL] generalOffset({0}) = CurrFreq {1} - gui::waterfall.getCenterFrequency() {2}, bookmarkName  = {3}", generalOffset, CurrFreq, mainFreq, bookmarkName);
                    ImGui::WaterfallVFO *vfo = nullptr;
                    auto it = gui::waterfall.vfos.find(bookmarkName);
                    if (it != gui::waterfall.vfos.end())
                    {
                        vfo = it->second;
                    }
                    else
                    {
                        flog::warn("[CONTROL] VFO for {0} not found in gui::waterfall.vfos", bookmarkName);
                    }

                    if (vfo != nullptr)
                    {
                        vfo->setOffset(generalOffset); // Добавлен флаг mainVFO = true
                        flog::info("[CONTROL] bookmarkName {0}, Offset {1}", bookmarkName, generalOffset);
                        core::configManager.conf["vfoOffsets"][bookmarkName] = generalOffset;
                    }
                }
            }
        }
        if (modif_vfo)
            core::configManager.release(true);
        config.release();

        auto ctm = currentTimeMillis();
        for (auto &tr : transientBookmarks)
        {
            if (ctm < tr.notValidAfter)
            {
                waterfallBookmarks.push_back(tr);
            }
        }
    }

    void loadFirst()
    {
        if (listNames.size() > 0)
        {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName)
    {
        bookmarks.clear();

        // Проверяем, существует ли вообще такой список. Если нет - загружаем первый доступный.
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end())
        {
            selectedListName = "";
            selectedListId = 0;
            loadFirst(); // Предполагается, что эта функция загружает первый/дефолтный список
            return;
        }

        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;

        config.acquire();
        try
        {
            // Дополнительная проверка, что путь к закладкам существует и является объектом
            if (!config.conf.contains("lists") ||
                !config.conf["lists"].contains(listName) ||
                !config.conf["lists"][listName].contains("bookmarks") ||
                !config.conf["lists"][listName]["bookmarks"].is_object())
            {
                flog::warn("List '{0}' or its 'bookmarks' section not found or is not a valid object.", listName);
                config.release();
                bookmarks_size = bookmarks.size(); // Обновляем размер (будет 0)
                return;
            }

            // Создаем ссылку для удобства и читаемости
            const auto &bookmarks_json = config.conf["lists"][listName]["bookmarks"];

            for (auto &[bmName, bm] : bookmarks_json.items())
            {
                // Пропускаем некорректные записи (не объекты)
                if (!bm.is_object())
                    continue;

                ObservationBookmark fbm;
                fbm.frequency = bm.value("frequency", 0.0);

                // Закладка без частоты бессмысленна, пропускаем ее
                if (fbm.frequency <= 0.0)
                {
                    flog::warn("Skipping bookmark '{0}' in list '{1}' due to invalid frequency.", bmName, listName);
                    continue;
                }
                fbm.bandwidth = bm.value("bandwidth", 12500.0);
                fbm.mode = bm.value("mode", 0);
                fbm.scard = bm.value("scard", "");
                fbm.level = std::clamp(bm.value("level", -70), -150, -30);
                fbm.dopinfo = bm.value("dopinfo", bmName);
                fbm.Signal = bm.value("Signal", 0);

                fbm.selected = false;
                bookmarks[bmName] = fbm;
            }
        }
        catch (const nlohmann::json::exception &e)
        {
            // Этот catch теперь будет ловить только серьезные ошибки парсинга,
            // а не просто отсутствующие ключи.
            flog::error("A critical error occurred while parsing bookmarks for list '{0}': {1}", listName, e.what());
        }
        config.release();

        bookmarks_size = bookmarks.size();
    }

    void loadListToInstance(std::string listName)
    {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end())
        {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items())
        {
            try
            {
                if (bm["frequency"] == "")
                    break;
            }
            catch (...)
            {
                flog::warn("List {0} is emply", listName);
                break;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];

            try
            {
                fbm.scard = bm["scard"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.scard = "";
            }
            try
            {
                fbm.level = bm["level"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.level = -70;
            }
            fbm.level = std::clamp<int>(fbm.level, -150, -30);
            try
            {
                fbm.dopinfo = bm["dopinfo"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.dopinfo = bmName;
            }
            try
            {
                fbm.Signal = bm["Signal"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.Signal = 0;
            }
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
        bookmarks_size = bookmarks.size();
    }

    void saveByName(std::string listName)
    {
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks)
        {
            // flog::info("saveByName {0}", bm.dopinfo);
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = bm.mode;
            config.conf["lists"][listName]["bookmarks"][bmName]["level"] = bm.level;
            config.conf["lists"][listName]["bookmarks"][bmName]["scard"] = bm.scard;
            config.conf["lists"][listName]["bookmarks"][bmName]["dopinfo"] = bm.dopinfo;
            config.conf["lists"][listName]["bookmarks"][bmName]["Signal"] = bm.Signal;
        }
        config.release(true);
    }

    bool AddSelectedList_Short()
    {
        // Update enabled and disabled modules
        core::configManager.acquire();
        json instances;
        for (auto [_name, inst] : core::moduleManager.instances)
        {
            instances[_name]["module"] = inst.module.info->name;
            instances[_name]["enabled"] = inst.instance->isEnabled();
        }
        core::configManager.conf["moduleInstances"] = instances;
        core::configManager.release(true);
        return true;
    }

    bool AddSelectedList(bool _fake = false)
    {
        flog::info("AddSelectedList");
        if (isARM)
            return true;
        for (auto [bmName, bm] : config.conf["lists"][selectedListName]["bookmarks"].items())
        {
            try
            {
                if (bm["frequency"] == "")
                    break;
            }
            catch (...)
            {
                flog::warn("loadByName listName {0} is emply", selectedListName);
                break;
            }
            std::string _name = bmName;
            if (_fake == false)
            {
                flog::info("TRACE! Add Instance {0}", _name);
                if (!core::moduleManager.createInstance(_name, "radio"))
                {
                    // flog::info("!!! 1398");
                    core::moduleManager.postInit(_name);
                    try
                    {
                        int mode = (int)bm["mode"];
                        double bandwidth = (double)bm["bandwidth"];
                        flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 1452");
                        core::modComManager.callInterface(_name, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                        core::modComManager.callInterface(_name, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                    }
                    catch (...)
                    {
                    }
                }
                else
                {
                    flog::error("!!! ERROR adding Radio. modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", _name, editedBookmarkName, editedBookmark.frequency);
                }
            }
            _name = "Запис " + _name;
            flog::info("TRACE! Add Instance {0}", _name);
            if (!core::moduleManager.createInstance(_name, "recorder"))
            {
                // flog::info("!!! 1411");
                core::moduleManager.postInit(_name);
                core::modComManager.callInterface(_name, RECORDER_IFACE_CMD_SET_STREAM, (void *)editedBookmarkName.c_str(), NULL);

                flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", _name, editedBookmarkName, editedBookmark.frequency);
            }
            else
            {
                flog::error("!!! ERROR adding Запис (Recorder)! modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", _name, editedBookmarkName, editedBookmark.frequency);
            }
        }
        if (_fake == false)
        {
            // Update enabled and disabled modules
            core::configManager.acquire();
            json instances;
            for (auto [_name, inst] : core::moduleManager.instances)
            {
                instances[_name]["module"] = inst.module.info->name;
                instances[_name]["enabled"] = inst.instance->isEnabled();
            }
            core::configManager.conf["moduleInstances"] = instances;
            core::configManager.release(true);
        }
        return true;
    }

    // В файле supervision4.cpp

    bool DelSelectedList(bool _fake = false)
    {
        flog::info("DelSelectedList, _fake: {0}", _fake);
        if (isARM)
        {
            return true; // На АРМ эта логика не нужна
        }

        // ===================================================================
        // ШАГ 1: БЕЗОПАСНЫЙ сбор имен для удаления.
        // Мы блокируем конфиг только на очень короткое время, чтобы скопировать имена.
        // ===================================================================
        std::vector<std::string> instances_to_delete;
        std::vector<std::string> rec_instances_to_delete;

        config.acquire(); // Блокируем конфиг ПЕРЕД любым чтением
        try
        {
            // Проверяем, что все ключи существуют, чтобы избежать падения
            if (config.conf.contains("lists") &&
                config.conf["lists"].contains(selectedListName) &&
                config.conf["lists"][selectedListName].contains("bookmarks"))
            {
                // Собираем имена из списка закладок
                for (auto const &[bmName, bm] : config.conf["lists"][selectedListName]["bookmarks"].items())
                {
                    if (bmName == "Канал приймання")
                    {
                        continue;
                    }
                    instances_to_delete.push_back(bmName);
                    rec_instances_to_delete.push_back("Запис " + bmName);
                }
            }

            // Этот блок (_fake == false) вызывается при полной остановке/очистке.
            // Он добавляет стандартные имена, чтобы гарантировать полную очистку.
            if (_fake == false)
            {
                for (int i = 1; i <= MAX_CHANNELS; i++) // Используем MAX_CHANNELS вместо жестко заданного 8
                {
                    std::string _name = "C" + std::to_string(i);
                    // Добавляем, только если еще не в списке, чтобы избежать дубликатов
                    if (std::find(instances_to_delete.begin(), instances_to_delete.end(), _name) == instances_to_delete.end())
                    {
                        instances_to_delete.push_back(_name);
                        rec_instances_to_delete.push_back("Запис " + _name);
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            // Если при чтении конфига произошла ошибка, логируем ее и безопасно выходим
            flog::error("Error gathering instance names in DelSelectedList: {0}", e.what());
            config.release(); // Обязательно освобождаем мьютекс перед выходом!
            return false;
        }
        config.release(); // Освобождаем конфиг. Теперь другие потоки могут с ним работать.

        // ===================================================================
        // ШАГ 2: Удаление модулей (может быть долгим процессом).
        // На этом этапе конфиг уже не заблокирован.
        // ===================================================================
        flog::info("DelSelectedList: Deleting recorder instances...");
        for (const auto &name : rec_instances_to_delete)
        {
            if (core::moduleManager.instanceExist(name))
            {
                core::moduleManager.deleteInstance(name);
            }
        }

        flog::info("DelSelectedList: Deleting radio instances...");
        for (const auto &name : instances_to_delete)
        {
            if (core::moduleManager.instanceExist(name))
            {
                core::moduleManager.deleteInstance(name);
            }
        }

        // ===================================================================
        // ШАГ 3: Финальное обновление конфига ПОСЛЕ удаления всех модулей.
        // ===================================================================
        flog::info("DelSelectedList: Updating configuration...");
        core::configManager.acquire(); // Снова блокируем конфиг для записи
        try
        {
            // Обновляем JSON-конфиг на основе ТЕКУЩЕГО состояния moduleManager
            json instances;
            for (auto const &[_name, inst] : core::moduleManager.instances)
            {
                instances[_name]["module"] = inst.module.info->name;
                instances[_name]["enabled"] = inst.instance->isEnabled();
            }
            core::configManager.conf["moduleInstances"] = instances;

            // Безопасно очищаем смещения VFO
            if (core::configManager.conf.contains("vfoOffsets"))
            {
                for (int i = 1; i <= MAX_CHANNELS; i++)
                {
                    core::configManager.conf["vfoOffsets"]["C" + std::to_string(i)] = 0.0;
                }
            }
        }
        catch (const std::exception &e)
        {
            flog::error("Exception during final config update in DelSelectedList: {0}", e.what());
        }
        core::configManager.release(true); // Освобождаем и СОХРАНЯЕМ конфиг на диск
        flog::info("DelSelectedList: Config updated.");

        // ===================================================================
        // ШАГ 4: Работа с GUI (не требует блокировок менеджеров)
        // ===================================================================
        for (int i = 1; i <= MAX_CHANNELS; i++)
        {
            gui::waterfall.vfos.erase("C" + std::to_string(i));
        }

        flog::info("DelSelectedList finished.");
        return true;
    }

    bool AddSelectedFreq(std::string _name)
    {
        if (isARM)
            return true;
        flog::info("TRACE! AddSelectedFreq {0}", _name);
        if (!core::moduleManager.createInstance(_name, "radio"))
        {
            // flog::info("!!! 1482");
            core::moduleManager.postInit(_name);
            int mode = editedBookmark.mode;
            double bandwidth = editedBookmark.bandwidth;
            flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 1542");
            core::modComManager.callInterface(_name, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(_name, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
            flog::info("!!! Канал приймання successfully added! modName={0}, editedBookmarkName={1}", _name, editedBookmarkName);
        }
        else
        {
            flog::error("!!! ERROR adding Radio. modName={0}, editedBookmarkName={1}", _name, editedBookmarkName);
        }

        _name = "Запис " + _name;
        flog::info("TRACE! Add Instance {0}", _name);
        if (!core::moduleManager.createInstance(_name, "recorder"))
        {
            // flog::info("!!! 1496");
            core::moduleManager.postInit(_name);
            core::modComManager.callInterface(_name, RECORDER_IFACE_CMD_SET_STREAM, (void *)editedBookmarkName.c_str(), NULL);
            flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} ", _name, editedBookmarkName);
        }
        else
        {
            flog::error("!!! ERROR adding Запис (Recorder)! modName={0}, editedBookmarkName={1}", _name, editedBookmarkName);
        }
        // ARMinstances[_name]["module"]="radio";
        // ARMinstances[_name]["enabled"]="false"

        // Update enabled and disabled modules
        core::configManager.acquire();
        json instances;
        for (auto [_name, inst] : core::moduleManager.instances)
        {
            instances[_name]["module"] = inst.module.info->name;
            instances[_name]["enabled"] = inst.instance->isEnabled();
        }
        core::configManager.release(true);
        return true;
    }

    bool DelSelectedFreq(std::string _name)
    {
        flog::info("DelSelectedFreq {0}", _name);
        if (isARM)
            return true;
        sigpath::sinkManager.stopStream(_name);
        // flog::info("1");
        std::string rec_name = "Запис " + _name;
        core::moduleManager.deleteInstance(rec_name);
        // flog::info("2");
        // sigpath::sinkManager.unregisterStream(_name);
        core::moduleManager.deleteInstance(_name);

        // flog::info("3");
        // Update enabled and disabled modules
        core::configManager.acquire();
        json instances;
        for (auto [_name, inst] : core::moduleManager.instances)
        {
            instances[_name]["module"] = inst.module.info->name;
            instances[_name]["enabled"] = inst.instance->isEnabled();
            flog::info("... {0}", inst.module.info->name);
        }
        core::configManager.conf["moduleInstances"] = instances;
        core::configManager.release(true);
        // flog::info("End");
        return true;
    }

    static void menuHandler(void *ctx)
    {
        SupervisorModeModule *_this = (SupervisorModeModule *)ctx;
        if (gui::mainWindow.getStopMenuUI())
        {
            return;
        }
        float menuWidth = ImGui::GetContentRegionAvail().x;
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float btnSize = 0;
        _this->currSource = sourcemenu::getCurrSource();
        uint8_t currSrvr = gui::mainWindow.getCurrServer();
        _this->CurrSrvr = currSrvr;
        ModuleState actual_state = _this->currentState.load();

        int _work = 0; // setRecording(recording.load());
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
        if (actual_state == ModuleState::RUNNING && !gui::mainWindow.isPlaying())
        {
            gui::mainWindow.setServerRecordingStop(0);
            gui::mainWindow.setRecording(false);
            _this->stop();
        }
        std::string selVFO = gui::waterfall.selectedVFO;
        bool _run = (actual_state != ModuleState::STOPPED);

        bool _update_ListID = false;
        if (_this->isARM)
        {
            _this->status_auto_level4 = gui::mainWindow.getAuto_levelCtrl(currSrvr);
            _this->maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_ctrl(currSrvr);

            if (gui::mainWindow.getUpdateMenuSnd0Main(currSrvr))
            {
                _this->selectedListId = gui::mainWindow.getidOfList_ctrl(currSrvr);
                _update_ListID = true;
            }
        }

        bool showThisMenu = _this->isServer;
        if (_this->Admin)
            showThisMenu = false;
        /// flog::info("run {0} || selVFO {1} || _work {2}, showThisMenu {3}", _run, selVFO, _work, showThisMenu);
        // run false || selVFO Канал приймання || _work 1, showThisMenu false
        if (_run || selVFO != "Канал приймання" || _work > 0)
        {
            ImGui::BeginDisabled();
        }
        {
            // TODO: Replace with something that won't iterate every frame
            std::vector<std::string> selectedNames;
            for (auto &[name, bm] : _this->bookmarks)
            {
                if (bm.selected)
                {
                    selectedNames.push_back(name);
                }
            }

            if (!showThisMenu)
                btnSize = ImGui::CalcTextSize("Зберегти як...").x + 8;
            ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
            if (showThisMenu)
            {
                ImGui::BeginDisabled();
            }
            {
                if (ImGui::Combo(("##supervisor_list_sel_4" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str()) || _update_ListID)
                {
                    _update_ListID = false;
                    // flog::info("1 .. _this->selectedListName {0}, new {1}", _this->selectedListName, _this->listNames[_this->selectedListId]);
                    if (_this->listNames[_this->selectedListId] != _this->selectedListName)
                    {

                        /// DMH _this->DelSelectedList();
                        config.acquire();
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                        config.release(true);
                        // _this->refreshWaterfallBookmarks();

                        /// DMH _this->AddSelectedList();

                        _this->loadByName(_this->listNames[_this->selectedListId]);
                        config.acquire();
                        config.conf["selectedList"] = _this->listNames[_this->selectedListId]; //_this->selectedListName;
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                        config.release(true);
                        _this->refreshWaterfallBookmarks();
                        // _this->genlevel = config.conf["lists"][_this->selectedListName]["genlevel"];
                        /// core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);
                        _this->saveByName(_this->selectedListName);
                        _this->refreshLists();

                        double curr_freq = 0;
                        for (auto [bmName, bm] : _this->bookmarks)
                        {
                            curr_freq = bm.frequency;
                            break;
                        }
                        flog::info(" selectedListId {0}, _this->selectedListName {1}, curr_freq {2}", _this->selectedListId, _this->selectedListName, curr_freq);
                        if (!gui::waterfall.selectedVFO.empty())
                            tuner::centerTuning(gui::waterfall.selectedVFO, curr_freq);
                        // int tuningMode = tuner::TUNER_MODE_NORMAL;
                        tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, gui::freqSelect.frequency);
                        gui::waterfall.centerFreqMoved = true;
                        gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);

                        gui::mainWindow.setidOfList_ctrl(currSrvr, _this->selectedListId);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                        gui::mainWindow.setUpdateCTRLListForBotton(currSrvr, true);
                    }
                }
            }
            if (showThisMenu)
            {
                ImGui::EndDisabled();
            }
            if (!showThisMenu)
            { // if (!_this->isServer) {
                bool FirstBank = (_this->listNames[_this->selectedListId] == "General") ? true : false;
                bool ActiveUseBank = false;
                for (int i = 0; i < MAX_SERVERS; i++)
                {
                    if (gui::mainWindow.getbutton_ctrl(i) == true && gui::mainWindow.getidOfList_ctrl(i) == _this->selectedListId)
                    {
                        ActiveUseBank = true;
                        break;
                    }
                }

                ImGui::SameLine();
                if (_this->listNames.size() == 0 || FirstBank || ActiveUseBank)
                {
                    style::beginDisabled();
                }
                if (ImGui::Button(("Зберегти як...##supervisor_ren_lst_4" + _this->name).c_str(), ImVec2(btnSize, 0)))
                {
                    _this->firstEditedListName = _this->listNames[_this->selectedListId];
                    _this->editedListName = _this->firstEditedListName;
                    _this->renameListOpen = true;
                }
                if (_this->listNames.size() == 0 || FirstBank || ActiveUseBank)
                {
                    style::endDisabled();
                }
                ImGui::SameLine();
                if (ImGui::Button(("+##supervisor_add_lst_4" + _this->name).c_str(), ImVec2(lineHeight, 0)))
                {
                    /// DMH _this->DelSelectedList();
                    /// DMH config.acquire();
                    /// DMH config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                    /// DMH config.release(true);
                    /// DMH  _this->refreshWaterfallBookmarks(true);

                    /// DMH core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);

                    // Find new unique default name
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), "Новий банк") == _this->listNames.end())
                    {
                        _this->editedListName = "Новий банк";
                    }
                    else
                    {
                        char buf[64];
                        for (int i = 1; i < 1000; i++)
                        {
                            sprintf(buf, "Новий банк (%d)", i);
                            if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end())
                            {
                                break;
                            }
                        }
                        _this->editedListName = buf;
                        bool _record = true;
                    }
                    _this->newListOpen = true;
                }
                ImGui::SameLine();
                if (_this->selectedListName == "" || FirstBank || ActiveUseBank)
                {
                    style::beginDisabled();
                }
                if (ImGui::Button(("-##supervisor__del_lst_4" + _this->name).c_str(), ImVec2(lineHeight, 0)))
                {
                    _this->deleteListOpen = true;
                }
                if (_this->selectedListName == "" || FirstBank || ActiveUseBank)
                {
                    style::endDisabled();
                }

                // List delete confirmation
                if (ImGui::GenericDialog(("supervisor_del_list_confirm_4" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]()
                                         { ImGui::Text("Видалення банку \"%s\". Ви впевнені?", _this->selectedListName.c_str()); }) == GENERIC_DIALOG_BUTTON_YES)
                {
                    if (_this->selectedListName != "General")
                    {
                        // _this->DelSelectedList(); /// DMH
                        // _this->saveByName(_this->selectedListName);

                        std::vector<std::string> listNamesCopy = _this->listNames;
                        std::map<int, std::string> oldSelectedNames;
                        for (int i = 0; i < MAX_SERVERS; ++i)
                        {
                            int listId = gui::mainWindow.getidOfList_ctrl(i);
                            if (listId >= 0 && listId < static_cast<int>(listNamesCopy.size()))
                            {
                                oldSelectedNames[i] = listNamesCopy[listId];
                            }
                        }
                        _this->listUpdater.store(true);
                        config.acquire();
                        config.conf["lists"].erase(_this->selectedListName);
                        config.release(true);
                        _this->refreshWaterfallBookmarks();

                        _this->refreshLists();
                        _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
                        if (_this->listNames.size() > 0)
                        {
                            _this->loadByName(_this->listNames[_this->selectedListId]);
                        }
                        else
                        {
                            _this->selectedListName = "";
                        }
                        /// DMH _this->AddSelectedList();

                        config.acquire();
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                        config.release(true);
                        _this->refreshWaterfallBookmarks();
                        /// DMH core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);

                        _this->saveByName(_this->selectedListName);
                        _this->refreshLists();
                        _this->getScanLists();
                        _this->restoreListSelections_forceRemap(oldSelectedNames, _this->listNames, _this->listNamesTxt);
                        gui::mainWindow.setUpdateCTRLListForBotton(_this->CurrSrvr, true);
                        gui::mainWindow.setidOfList_ctrl(_this->CurrSrvr, _this->selectedListId);
                        gui::mainWindow.setflag_level_ctrl(_this->CurrSrvr, _this->getflag_level[_this->CurrSrvr]);
                        gui::mainWindow.setLevelDbCtrl(_this->CurrSrvr, _this->getgen_level[_this->CurrSrvr]);
                        gui::mainWindow.setAKFInd_ctrl(_this->CurrSrvr, _this->status_AKF);
                        gui::mainWindow.setCTRLListNamesTxt(_this->listNamesTxt);
                        gui::mainWindow.setUpdateModule_ctrl(_this->CurrSrvr, true);
                        gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
                        _this->listUpdater.store(false);
                    }
                }
            }
            if (_this->selectedListName == "")
            {
                style::beginDisabled();
            }

            // Draw import and export buttons

            if (showThisMenu)
            {
                ImGui::BeginDisabled();
            } //---------------------------------
            ImGui::BeginTable(("supervisor_bottom_btn_table_4" + _this->name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // Імпорт
            if (ImGui::Button(("Завантажити##supervisor_imp_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen)
            {
                _this->importOpen = true;
                std::string pathValid = ""; //  core::configManager.getPath() +"/Banks/*";
                core::configManager.acquire();
                std::string jsonPath = core::configManager.conf["PathJson"];
                core::configManager.release();
                try
                {
                    fs::create_directory(jsonPath);
                    pathValid = jsonPath + "/Control";
                    fs::create_directory(pathValid);
                    pathValid = pathValid + "/*";
                }
                catch (const std::exception &e)
                {
                    _this->importOpen = false;
                    std::cerr << e.what() << '\n';
                }
                if (_this->importOpen)
                    _this->importDialog = new pfd::open_file("Import bookmarks", pathValid, {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
            }

            ImGui::TableSetColumnIndex(1);
            if (_this->selectedListName == "")
            {
                style::beginDisabled();
            } // selectedNames.size() == 0 &&
            // Експорт
            if (ImGui::Button(("Зберегти у файл##supervisor_exp_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen)
            {
                std::string pathValid = core::configManager.getPath() + "/Banks/";
                core::configManager.acquire();
                int InstNum = 0;
                try
                {
                    std::string jsonPath = core::configManager.conf["PathJson"];
                    flog::info("jsonPath '{0}'", jsonPath);
                    InstNum = core::configManager.conf["InstanceNum"];
                    fs::create_directory(jsonPath);
                    pathValid = jsonPath + "/Control";
                    fs::create_directory(pathValid);
                }
                catch (const std::exception &e)
                {
                    std::cerr << e.what() << '\n';
                    pathValid = core::configManager.getPath() + "/Banks";
                }
                core::configManager.release();

                try
                {
                    _this->exportedBookmarks = json::object();
                    std::string expname = pathValid + "/control_" + std::to_string(InstNum) + "_" + _this->selectedListName + ".json";
                    _this->exportedBookmarks["domain"] = "freqs-bank";
                    _this->exportedBookmarks["rx-mode"] = "observation";
                    _this->exportedBookmarks["bank-name"] = _this->selectedListName;

                    time_t rawtime;
                    struct tm *timeinfo;
                    char buffer[80];
                    time(&rawtime);
                    timeinfo = localtime(&rawtime);
                    strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
                    std::string s(buffer);

                    _this->exportedBookmarks["time_created"] = s;
                    _this->exportedBookmarks["InstNum"] = InstNum;

                    config.acquire();
                    // _this->exportedBookmarks[_this->selectedListName]["InstNum"] = InstNum;
                    for (auto [bmName, bm] : config.conf["lists"][_this->selectedListName]["bookmarks"].items())
                    {
                        _this->exportedBookmarks[_this->selectedListName]["bookmarks"][bmName] = config.conf["lists"][_this->selectedListName]["bookmarks"][bmName];
                    }
                    config.release();
                    // flog::info("4. expname {0}", expname);

                    _this->exportOpen = true;
                    _this->exportDialog = new pfd::save_file("Export bookmarks", expname.c_str(), {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                }
                catch (const std::exception &e)
                {
                    std::cerr << e.what() << '\n';
                }
            }

            if (_this->selectedListName == "")
            {
                style::endDisabled();
            } // selectedNames.size() == 0 &&
            ImGui::EndTable();
            if (showThisMenu)
            {
                ImGui::EndDisabled();
            } //---------------------------------

            // if (_this->isServer) { ImGui::BeginDisabled(); } //---------------------------------
            {
                if (ImGui::BeginTable(("supervisor_bkm_table_4" + _this->name).c_str(), 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 200)))
                {                                            // | ImGuiTableFlags_ScrollY
                                                             //            gui::waterfall.selectedVFO = "Канал приймання";
                    ImGui::TableSetupColumn("Інд.");         //, ImGuiTableColumnFlags_WidthFixed, 20.0f
                    ImGui::TableSetupColumn("Назва");        // , ImGuiTableColumnFlags_WidthFixed, 40.0f
                    ImGui::TableSetupColumn("Частота, МГц"); // ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Вид демод.");
                    ImGui::TableSetupColumn("Смуга, кГц");
                    // ImGui::TableSetupColumn("Поріг, дБ");
                    ImGui::TableSetupColumn("Тип сигналу");
                    ImGui::TableSetupColumn("Запис"); // , ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupScrollFreeze(2, 1);
                    ImGui::TableHeadersRow();
                    int i = 0;
                    for (auto &[name, bm] : _this->bookmarks)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImVec2 min = ImGui::GetCursorPos();
                        if (ImGui::Selectable((name + "##supervisor_bkm_name_4" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick))
                        {
                            // if shift or control isn't pressed, deselect all others
                            if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl)
                            {
                                for (auto &[_name, _bm] : _this->bookmarks)
                                {
                                    if (name == _name)
                                    {
                                        continue;
                                    }
                                    _bm.selected = false;
                                }
                            }
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        { // ImGui::TableGetHoveredColumn() >= 0 &&
                            applyBookmark(bm, gui::waterfall.selectedVFO);
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", bm.dopinfo.c_str());

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%s", utils::formatFreqMHz(bm.frequency).c_str());

                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%s", demodModeList[bm.mode]);

                        // ImGui::Text("%s %s", utils::formatFreqMHz(bm.frequency).c_str(), demodModeList[bm.mode]);

                        ImGui::TableSetColumnIndex(4);
                        int bw = round(bm.bandwidth / 1000);
                        std::string sbw = std::to_string(bw);
                        if (bw == 13)
                            sbw = "12.5";
                        if (bw == 6)
                            sbw = "6.25";
                        ImGui::Text("%s", sbw.c_str());

                        ImGui::TableSetColumnIndex(5);
                        if (bm.Signal == 2)
                        {
                            ImGui::Text("DMR");
                        }
                        else if (bm.Signal == 1)
                        {
                            ImGui::Text("ТЛФ");
                        }
                        else
                        {
                            ImGui::Text("Авто");
                        }
                        // ImGui::Text("%s", std::to_string(bm.Signal).c_str());

                        ImGui::TableSetColumnIndex(6);
                        if (_this->isARM || actual_state == ModuleState::STOPPED)
                        {
                            ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", " ");
                        }
                        else
                        {
                            if (_this->ch_recording[i] == true)
                                ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", "Так");
                            // ImGui::Text("Так");
                            else
                                ImGui::TextColored(ImVec4(1, 0, 1, 1), "%s", "Стоп");
                        }
                        ImVec2 max = ImGui::GetCursorPos();
                        i++;
                    }
                    ImGui::EndTable();
                }
                if (!showThisMenu)
                {
                    // Draw buttons on top of the list
                    ImGui::BeginTable(("supervisor_btn_table" + _this->name).c_str(), 3);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);

                    if (_this->bookmarks.size() >= _this->maxCHANNELS)
                    { //
                        style::beginDisabled();
                        ImGui::Button(("Додати##supervisor_add_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0));
                        style::endDisabled();
                        // ImGui::GenericDialog("##sdrpp_srv_src_err_dialog4", serverBusy, GENERIC_DIALOG_BUTTONS_OK, [=](){
                        //     ImGui::TextUnformatted("This server is already in use.");
                        // });
                    }
                    else
                    {
                        //=========================================================================
                        if (ImGui::Button(("Додати##supervisor_add_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                        {
                            char modName[1024];
                            // If there's no VFO selected, just save the center freq
                            _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                            _this->editedBookmark.mode = 7;
                            if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) != "")
                            {
                                int mode = gui::mainWindow.getselectedDemodID();
                                // core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                                _this->editedBookmark.mode = mode;
                            }
                            _this->_frec = _this->editedBookmark.frequency / 1000.0; //  round(_this->editedBookmark.frequency);

                            float bw = _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

                            _this->editedBookmark.bandwidth = bw;

                            _this->_raw = false;

                            // Get FFT data
                            int Signal = 0;
                            float maxLevel = _this->getgen_level[currSrvr];
                            int dataWidth = 0;
                            float *data = gui::waterfall.acquireLatestFFT(dataWidth);
                            if (data)
                            {
                                // Get gather waterfall data
                                double wfCenter = _this->editedBookmark.frequency;
                                double wfWidth = _this->editedBookmark.bandwidth;
                                double wfStart = wfCenter - (wfWidth / 2.0);
                                double wfEnd = wfCenter + (wfWidth / 2.0);
                                // Gather VFO data
                                double vfoWidth = _this->editedBookmark.bandwidth;

                                // Check if we are waiting for a tune
                                maxLevel = _this->getMaxLevel(data, _this->current, vfoWidth, dataWidth, wfStart, wfWidth) + 5;
                            }
                            // Release FFT Data
                            gui::waterfall.releaseLatestFFT();

                            flog::info("maxLevel = {0}", maxLevel);

                            _this->editedBookmark.level = maxLevel;
                            _this->editedBookmark.Signal = Signal;
                            int curr_frequency = round(_this->editedBookmark.frequency);
                            _this->editedBookmark.scard = "";
                            // _this->editedBookmarkName =  std::to_string(curr_frequency);
                            _this->editedBookmark.selected = false;
                            // Find new unique default name
                            if (_this->bookmarks.find("C1") == _this->bookmarks.end())
                            {
                                _this->editedBookmarkName = "C1";
                            }
                            else
                            {
                                char buf[64];
                                bool _flag = false;
                                for (int i = 2; i <= MAX_CHANNELS; i++)
                                {
                                    sprintf(buf, "C%d", i);
                                    if (_this->bookmarks.find(buf) == _this->bookmarks.end())
                                    {
                                        _flag = true;
                                        break;
                                    }
                                }
                                if (_flag)
                                    _this->editedBookmarkName = buf;
                            }
                            _this->dop_info = _this->editedBookmarkName;
                            _this->createOpen = true;
                        }
                        //=========================================================================
                    }
                    ImGui::TableSetColumnIndex(1);
                    //=========================================================================
                    if (selectedNames.size() == 0 && _this->selectedListName != "")
                    {
                        style::beginDisabled();
                    }
                    if (ImGui::Button(("Видалити##supervisor_rem_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    {
                        _this->deleteBookmarksOpen = true;
                        _this->editedBookmark = _this->bookmarks[selectedNames[0]];
                    }
                    if (selectedNames.size() == 0 && _this->selectedListName != "")
                    {
                        style::endDisabled();
                    }
                    ImGui::TableSetColumnIndex(2);
                    //=========================================================================
                    if (selectedNames.size() != 1 && _this->selectedListName != "")
                    {
                        style::beginDisabled();
                    }
                    if (ImGui::Button(("Редаг##supervisor_edt_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    {
                        // _this->currDevList = _this->txtDevList;
                        _this->editedBookmark = _this->bookmarks[selectedNames[0]];
                        _this->editedBookmarkName = selectedNames[0];
                        _this->firstEditedBookmarkName = selectedNames[0];

                        _this->_frec = _this->editedBookmark.frequency / 1000.0; // round(_this->editedBookmark.frequency);
                        _this->_bandwidthId = -1;
                        _this->_raw = false;
                        for (int i = 0; i < _this->bandwidthsList.size(); i++)
                        {
                            if (_this->bandwidthsList[i] >= _this->editedBookmark.bandwidth)
                            {
                                _this->_bandwidthId = i;
                                break;
                            }
                        }
                        int i = 0;
                        _this->NumIDBookmark = 0;
                        for (auto &[name, bm] : _this->bookmarks)
                        {
                            if (bm.frequency == _this->editedBookmark.frequency && bm.mode == _this->editedBookmark.mode)
                            {
                                _this->NumIDBookmark = i;
                                flog::info("bm.frequency {0}, NumID {1}", bm.frequency, _this->NumIDBookmark);
                                break;
                            }
                            i++;
                        }
                        if (_this->_bandwidthId == -1)
                            _this->_bandwidthId = _this->bandwidthsList.size() - 1;

                        _this->dop_info = _this->editedBookmark.dopinfo;
                        _this->editOpen = true;
                    }
                    if (selectedNames.size() != 1 && _this->selectedListName != "")
                    {
                        style::endDisabled();
                    }

                    ImGui::EndTable();
                    if (_this->bookmarks.size() >= _this->maxCHANNELS)
                    {
                        std::string Str = "Ліміт каналів обмежений кількістю " + std::to_string(_this->maxCHANNELS) + "!";
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", Str.c_str());
                    }

                    // Bookmark delete confirm dialog
                    // List delete confirmation
                    if (ImGui::GenericDialog(("supervisor_del_list_confirm4" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]()
                                             { ImGui::TextUnformatted("Видалення вибраних частот. Ви впевнені?"); }) == GENERIC_DIALOG_BUTTON_YES)
                    {
                        // applyBookmark(_this->editedBookmark, gui::waterfall.selectedVFO);
                        for (auto &_name : selectedNames)
                        {
                            _this->DelSelectedFreq(_name);
                            _this->bookmarks.erase(_name);
                            _this->saveByName(_this->selectedListName);
                            _this->refreshLists();
                            // std::string vsink  = "vsink_" + std::to_string(_this->numInstance) + "_" + _this->editedBookmarkName;
                            //_this->unloadVirtCards(vsink);
                        }
                        core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);
                        /*
                        config.acquire();
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                        config.release(true);
                        */
                        _this->getScanLists();
                        // sigpath::sinkManager.setVolumeAndMute("Канал приймання", false, 0.1);
                        gui::mainWindow.setUpdateCTRLListForBotton(_this->CurrSrvr, true);
                        gui::mainWindow.setidOfList_ctrl(_this->CurrSrvr, _this->selectedListId);
                        gui::mainWindow.setflag_level_ctrl(_this->CurrSrvr, _this->getflag_level[_this->CurrSrvr]);
                        gui::mainWindow.setLevelDbCtrl(_this->CurrSrvr, _this->getgen_level[_this->CurrSrvr]);
                        gui::mainWindow.setAKFInd_ctrl(_this->CurrSrvr, _this->status_AKF);
                        gui::mainWindow.setCTRLListNamesTxt(_this->listNamesTxt);
                        gui::mainWindow.setUpdateModule_ctrl(_this->CurrSrvr, true);
                        gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
                        _this->refreshWaterfallBookmarks();
                    }
                }

                if (showThisMenu)
                {
                    ImGui::BeginDisabled();
                } //---------------------------------

                //---------------------------------------------------
                // if (!_run) { ImGui::BeginDisabled(); }
                if (_this->status_auto_level4 && !_this->getflag_level[currSrvr])
                {
                    _this->getflag_level[currSrvr] = true;
                    // _this->flag_level = _this->getflag_level[currSrvr];
                    gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrvr]);
                    // gui::mainWindow.setlevel_ctrl(currSrvr, _this->genlevel);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                }
                ImGui::LeftLabel("Час очікування, сек");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::InputInt("##linger_timeWait_ctrlner_4", &_this->maxRecWaitTime, 1, 180))
                {
                    flog::info("_this->maxRecWaitTime {0}", _this->maxRecWaitTime);
                    _this->maxRecWaitTime = std::clamp<int>(_this->maxRecWaitTime, 1, 180);
                    config.acquire();
                    if (_this->isARM)
                    {
                        if (currSrvr == 0)
                            config.conf["maxRecWaitTime_1"] = _this->maxRecWaitTime;
                        else if (currSrvr == 1)
                            config.conf["maxRecWaitTime_2"] = _this->maxRecWaitTime;
                        else if (currSrvr == 2)
                            config.conf["maxRecWaitTime_3"] = _this->maxRecWaitTime;
                        else if (currSrvr == 3)
                            config.conf["maxRecWaitTime_4"] = _this->maxRecWaitTime;
                        else if (currSrvr == 4)
                            config.conf["maxRecWaitTime_5"] = _this->maxRecWaitTime;
                        else if (currSrvr == 5)
                            config.conf["maxRecWaitTime_6"] = _this->maxRecWaitTime;
                        else if (currSrvr == 6)
                            config.conf["maxRecWaitTime_7"] = _this->maxRecWaitTime;
                        else if (currSrvr == 7)
                            config.conf["maxRecWaitTime_8"] = _this->maxRecWaitTime;
                    }
                    else
                    {
                        config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
                    }
                    config.release(true);

                    flog::info("_this->maxRecWaitTime {0}", _this->maxRecWaitTime);
                    gui::mainWindow.setMaxRecWaitTime_ctrl(currSrvr, _this->maxRecWaitTime);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                }
                if (ImGui::Checkbox("Автоматичне визначення порогу ##_auto_level_2", &_this->status_auto_level4))
                {
                    config.acquire();
                    if (_this->isARM)
                    {
                        if (currSrvr == 0)
                            config.conf["status_auto_level_1"] = _this->status_auto_level4;
                        else if (currSrvr == 1)
                            config.conf["status_auto_level_2"] = _this->status_auto_level4;
                        else if (currSrvr == 2)
                            config.conf["status_auto_level_3"] = _this->status_auto_level4;
                        else if (currSrvr == 3)
                            config.conf["status_auto_level_4"] = _this->status_auto_level4;
                        else if (currSrvr == 4)
                            config.conf["status_auto_level_5"] = _this->status_auto_level4;
                        else if (currSrvr == 5)
                            config.conf["status_auto_level_6"] = _this->status_auto_level4;
                        else if (currSrvr == 6)
                            config.conf["status_auto_level_7"] = _this->status_auto_level4;
                        else if (currSrvr == 7)
                            config.conf["status_auto_level_8"] = _this->status_auto_level4;
                    }
                    else
                    {
                        config.conf["status_auto_level"] = _this->status_auto_level4;
                    }
                    config.release(true);

                    _this->getflag_level[currSrvr] = true;

                    gui::mainWindow.setAuto_levelCtrl(currSrvr, _this->status_auto_level4);
                    if (_this->status_auto_level4)
                    {
                        _this->getflag_level[currSrvr] = true;
                        gui::mainWindow.setflag_level_ctrl(currSrvr, true);
                        gui::mainWindow.setLevelDbCtrl(currSrvr, _this->getgen_level[currSrvr]);
                    }
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                }
                _this->getflag_level[currSrvr] = true;
                /*
                if (!_this->isServer || _this->getflag_level[currSrvr] == true)
                {
                    if (ImGui::Checkbox("Загальний поріг##_supervision4__porig", &_this->getflag_level[currSrvr]))
                    {
                        gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrvr]);
                        gui::mainWindow.setLevelDbCtrl(currSrvr, _this->getgen_level[currSrvr]);
                        // gui::mainWindow.setlevel_ctrl(currSrvr, _this->genlevel);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                    }
                }
                if (_this->getflag_level[currSrvr])
                {
                */
                ImGui::LeftLabel("Поріг виявлення, дБ");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::SliderInt("##supervision4_level_4", &_this->getgen_level[currSrvr], -150, 0))
                {
                    gui::mainWindow.setLevelDbCtrl(currSrvr, _this->getgen_level[currSrvr]);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                }
                //}

                if (_this->SignalIndf)
                {
                    if (ImGui::Checkbox("Аналізувати АКФ##supervision4_auto_akf", &_this->status_AKF))
                    {
                        gui::mainWindow.setAKFInd_ctrl(currSrvr, _this->status_AKF);
                        _this->status_AKF = gui::mainWindow.getAKFInd_ctrl(currSrvr);
                        config.acquire();
                        config.conf["status_AKF"] = _this->status_AKF;
                        config.release(true);
                        flog::info("UPDATE _this->status_AKF {0}", _this->status_AKF);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrvr, true);
                    }
                    else
                    {
                        // _this->status_AKF = false;
                    }
                }

                if (showThisMenu)
                {
                    ImGui::EndDisabled();
                }
            }
            if (_run || selVFO != "Канал приймання" || _work > 0)
            {
                ImGui::EndDisabled();
            }
            //-----------------------------------------------------------------
        }

        //-----------------------------------------------------------------
        // if(gui::waterfall.selectedVFO!="Канал приймання") { style::beginDisabled(); }
        int _air_recording;
        bool _empty = ((_this->bookmarks_size == 0) ? true : false);
        bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();
        if (_this->currSource == SOURCE_ARM)
        {
            uint8_t currSrv = gui::mainWindow.getCurrServer();
             std::atomic<bool> running_arm = {false};
            running_arm.store(gui::mainWindow.getbutton_ctrl(currSrv));
            _air_recording = gui::mainWindow.isPlaying();
            bool rec_work = gui::mainWindow.getServerRecording(currSrv);

            int _control = gui::mainWindow.getServerStatus(currSrv);

            // flog::info("_this->running {0}, _air_recording {1}, _control {2}, _work {3}, rec_work {4}", _this->running_arm.store(), _air_recording, _control, _work, rec_work);
            if (_control != ARM_STATUS_FULL_CONTROL)
            {
                ImGui::BeginDisabled();
                ImGui::Button("СТАРТ##ctrl4_arm_start_1", ImVec2(menuWidth, 0));
                ImGui::EndDisabled();
            }
            else
            {
                // flog::info("_work {0} > 0 || rec_work {1} > 0 || _empty {2} || _ifStartElseBtn {3}", _work, rec_work, _empty, _ifStartElseBtn);
                if (!running_arm.load())
                {
                    if (_work > 0 || rec_work > 0 || _empty || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::BeginDisabled();
                    if (ImGui::Button("СТАРТ##ctrl4_arm_start_2", ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setbutton_ctrl(currSrv, true);
                        flog::info("СТАРТ currSrv {0}", currSrv);
                        gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrv]);
                        gui::mainWindow.setLevelDbCtrl(currSrvr, _this->getgen_level[currSrv]);
                        gui::mainWindow.setAKFInd_ctrl(currSrvr, _this->status_AKF);
                        gui::mainWindow.setUpdateListRcv7Ctrl(currSrv, true);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(currSrv, true);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                    }
                    if (_work > 0 || rec_work > 0 || _empty || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::EndDisabled();
                }
                else
                {
                    if (ImGui::Button("СТОП ##ctrl4_arm_start_3", ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrv]);
                        gui::mainWindow.setServerRecordingStop(currSrv);
                        // gui::mainWindow.setlevel_ctrl(currSrvr, _this->getgen_level[currSrv]);
                        gui::mainWindow.setAKFInd_ctrl(currSrvr, _this->status_AKF);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                        gui::mainWindow.setbutton_ctrl(currSrv, false);
                    }
                }
            }
        }
        else
        {
            core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
            if (!_run)
            {
                // flog::info("_work {0} > 0 || _this->isServer {1} > 0 || _empty {2} || _ifStartElseBtn {3}, _air_recording {4}", _work, _this->isServer, _empty, _ifStartElseBtn, _air_recording);
                // _work 1 > 0 || _this->isServer true > 0 || _empty false || _ifStartElseBtn false
                if (_work > 0 || _empty || (_this->isServer && _ifStartElseBtn))
                    ImGui::BeginDisabled();
                if (_air_recording == 1)
                {
                    if (ImGui::Button("СТАРТ##supervision4_start_4", ImVec2(menuWidth, 0)))
                    {
                        _this->start();
                    }
                }
                else
                {
                    style::beginDisabled();
                    ImGui::Button("СТАРТ##supervision4_start_4", ImVec2(menuWidth, 0));
                    style::endDisabled();
                }
                if (_work > 0 || _empty || (_this->isServer && _ifStartElseBtn))
                    ImGui::EndDisabled();

                ImGui::Text("Статус: Неактивний");
            }
            else
            {
                if (_air_recording == 0)
                {
                    gui::mainWindow.setServerRecordingStop(0);
                    gui::mainWindow.setRecording(false);
                    _this->stop();
                }
                if (ImGui::Button("СТОП ##supervision4_start_4", ImVec2(menuWidth, 0)))
                {
                    gui::mainWindow.setServerRecordingStop(0);
                    gui::mainWindow.setRecording(false);
                    _this->stop();
                }
                if (_this->_recording == true)
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", "Статус: Реєстрація");
                }
                else
                {
                    if (_run == true)
                    {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", "Статус: Приймання");
                    }
                    else
                    {
                        ImGui::Text("Статус: Неактивний");
                    }
                }
            }
        }

        if (_this->createOpen)
        {
            _this->createOpen = _this->bookmarkEditDialog();
        }

        if (_this->editOpen)
        {
            _this->editOpen = _this->bookmarkEditDialog();
        }

        if (_this->newListOpen)
        {
            _this->newListOpen = _this->newListDialog();
        }

        if (_this->renameListOpen)
        {
            _this->renameListOpen = _this->newListDialog();
        }

        if (_this->selectListsOpen)
        {
            _this->selectListsOpen = _this->selectListsDialog();
        }

        // Handle import and export
        if (_this->importOpen && _this->importDialog->ready())
        {

            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0)
            {
                try
                {
                    if (_this->importBookmarks(paths[0]))
                    {
                        /// DMH _this->DelSelectedList();
                        config.acquire();
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                        config.release(true);
                        _this->refreshWaterfallBookmarks(false);

                        /// DMH _this->AddSelectedList();

                        config.acquire();
                        config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                        config.release(true);
                        _this->refreshWaterfallBookmarks(false);

                        /// DMH core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);

                        _this->refreshLists();
                        _this->saveByName(_this->selectedListName);
                        _this->selectedListId = std::distance(_this->listNames.begin(), std::find(_this->listNames.begin(), _this->listNames.end(), _this->selectedListName));
                        flog::info("!!!! getCtrlLists() {0}, selectedListName {1}", _this->selectedListId, _this->selectedListName);
                        _this->bookmarks_size = _this->bookmarks.size();
                        _this->getScanLists();
                        gui::mainWindow.setUpdateCTRLListForBotton(currSrvr, true);
                        gui::mainWindow.setidOfList_ctrl(currSrvr, _this->selectedListId);
                        gui::mainWindow.setflag_level_ctrl(currSrvr, _this->getflag_level[currSrvr]);
                        gui::mainWindow.setLevelDbCtrl(currSrvr, _this->getgen_level[currSrvr]);
                        gui::mainWindow.setAKFInd_ctrl(currSrvr, _this->status_AKF);
                        gui::mainWindow.setCTRLListNamesTxt(_this->listNamesTxt);
                        gui::mainWindow.setUpdateModule_ctrl(currSrvr, true);
                        gui::mainWindow.setUpdateListRcv7Ctrl(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(MAX_SERVERS, true);
                    }
                }
                catch (const std::exception &e)
                {
                    // std::cerr << e.what() << '\n';
                    flog::error("{0}", e.what());
                    _this->txt_error = e.what();
                    _this->_error = true;
                }
            }
            delete _this->importDialog;
        }

        if (ImGui::GenericDialog(("manager_confirm4" + _this->name).c_str(), _this->_error, GENERIC_DIALOG_BUTTONS_OK, [_this]()
                                 { ImGui::TextColored(ImVec4(1, 0, 0, 1), "Помилка імпорту json!  %s", _this->txt_error.c_str()); }) == GENERIC_DIALOG_BUTTON_OK)
        {
            _this->importOpen = false;
            _this->_error = false;
            flog::warn("_this->_error {0}", _this->_error);
        };

        if (_this->exportOpen && _this->exportDialog->ready())
        {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "")
            {
                size_t pos = path.find(".json");
                if (pos == std::string::npos)
                {
                    path = path + ".json";
                    _this->exportDialog->result() = path;
                }
                if (_this->exportDialog->ready())
                {
                    _this->exportBookmarks(path);
                }
            }
            delete _this->exportDialog;
        }
    }
    //=====================================================================================================
    //=====================================================================================================
    //=====================================================================================================
    // Функция для определения границ поддиапазона
    void calculateScanSegment(double min_frec, double max_frec, const double band,
                              double &sigmentLeft, double &sigmentRight)
    {
        const int minScans = 20; // Минимальное количество сканирований

        // Текущая ширина диапазона
        double range = max_frec - min_frec;

        // Если min_frec == max_frec, расширяем диапазон симметрично от центра
        if (min_frec == max_frec)
        {
            double center = min_frec;
            sigmentLeft = center - (band * minScans / 2.0);
            sigmentRight = center + (band * minScans / 2.0);
            return;
        }

        // Проверяем, достаточно ли диапазона для 20+ сканирований
        if (range >= band * (minScans + 1))
        {
            // Идеальный случай: диапазон больше band * 21
            sigmentLeft = min_frec;
            sigmentRight = max_frec;
        }
        else if (range < band * minScans)
        {
            // Если диапазон меньше минимального (band * 20), расширяем его
            double neededRange = band * minScans;
            double center = (min_frec + max_frec) / 2.0;
            sigmentLeft = center - (neededRange / 2.0);
            sigmentRight = center + (neededRange / 2.0);
        }
        else
        {
            // Диапазон между band * 20 и band * 21 — используем исходные границы
            sigmentLeft = min_frec;
            sigmentRight = max_frec;
        }

        // Убеждаемся, что sigmentRight - sigmentLeft позволяет минимум 20 шагов
        if ((sigmentRight - sigmentLeft) < band * minScans)
        {
            double center = (sigmentLeft + sigmentRight) / 2.0;
            sigmentLeft = center - (band * minScans / 2.0);
            sigmentRight = center + (band * minScans / 2.0);
        }
    }
    // -----------
    void start()
    {
        if (isARM)
            return;
        flog::info("[SUPERVISOR] START");
        // std::lock_guard<std::mutex> lock(startMtx);
        ModuleState expected = ModuleState::STOPPED;
        if (currentState.compare_exchange_strong(expected, ModuleState::STARTING))
        {
            std::string folderPath = "%ROOT%/recordings";
            // std::string expandedPath = expandString(folderPath + genLogFileName("/observ_"));
            bookmarks_size = bookmarks.size();
            if (bookmarks_size == 0)
            {
                flog::warn("bookmarks is emply!");
                gui::mainWindow.setbutton_ctrl(gui::mainWindow.getCurrServer(), false);
                return;
            }
            gui::mainWindow.setStopMenuUI(true);
            core::configManager.acquire();
            if (core::configManager.conf.contains("menuElements") &&
                core::configManager.conf["menuElements"].is_array())
            {
                for (auto &element : core::configManager.conf["menuElements"])
                {
                    if (element.contains("name") && element["name"] == "Виводи ЦП")
                    {
                        element["open"] = false;
                        flog::info("GUI thread: вкладка 'Виводи ЦП' закрыта программно.");
                        break;
                    }
                }
            }

            // bool showMenu = core::configManager.conf["showMenu"];
            // core::configManager.conf["showMenu"] = false;
            try
            {
                source = core::configManager.conf["source"];
                radioMode = (int)core::configManager.conf["RadioMode"];
                maxRecDuration = core::configManager.conf["maxRecDuration"];
            }
            catch (const std::exception &e)
            {
            }
            core::configManager.release(true);

            clnt_mode = -1; //  || source == "Airspy"
            if (source == "Азалія-сервер")
            {
                clnt_mode = 0;
            }
            if (source == "Азалія-клієнт" || source == "Azalea Client")
            {
                clnt_mode = 1;
            }
            /*
            if (gui::mainWindow.isPlaying())
            {
                flog::info("[supervision4] Stopping player before reconfiguring VFOs...");
                gui::mainWindow.setPlayState(false);
                // Может понадобиться короткая пауза, чтобы все потоки гарантированно остановились
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            */
            // Запускаем тяжелую работу в отдельном потоке, чтобы не блокировать GUI
            std::thread([this]()
            {
                // ... Вся тяжелая логика из старой функции start() ...
                DelSelectedList(false); /// DMH
                /*
                config.acquire();
                // config.conf["lists"][selectedListName]["showOnWaterfall"] = false;
                config.release(true);
                */
                // refreshWaterfallBookmarks();

                loadByName(listNames[selectedListId]);
                AddSelectedList(); /// DMH

                config.acquire();
                // config.conf["lists"][selectedListName]["showOnWaterfall"] = true;
                config.conf["selectedList"] = selectedListName;
                config.release(true);

                CentralFreq = 0;
                if (clnt_mode == -1)
                {
                    double min_frec = 0;
                    double max_frec = 0;
                    int freq_count = 0;
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (bm.frequency > max_frec)
                            max_frec = bm.frequency;
                        if (bm.frequency < min_frec)
                            min_frec = bm.frequency;
                        if (min_frec == 0)
                            min_frec = bm.frequency;
                        if (max_frec == 0)
                            max_frec = bm.frequency;
                        freq_count++;
                        CentralFreq = bm.frequency;
                    }
                    if (freq_count > 1)
                    {
                        CentralFreq = (double)round(min_frec + (max_frec - min_frec) / 2);
                    }
                    // Set middle freq '210400000.000000, min_frec104300000.000000, max_frec107900000.000000!
                    // flog::info("Set middle freq = {0}, min_frec {1}, max_frec {2}! '", CentralFreq, min_frec, max_frec);
                    // gui::freqSelect.frequencyChanged = true;

                    calculateScanSegment(min_frec, max_frec, scan_band, sigmentLeft, sigmentRight);
                    gui::mainWindow.setupdateStart_ctrl(gui::mainWindow.getCurrServer(), false);
                    // gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
                    if (!gui::waterfall.selectedVFO.empty())
                        tuner::centerTuning(gui::waterfall.selectedVFO, CentralFreq);
                    // int tuningMode = tuner::TUNER_MODE_NORMAL;
                    tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, CentralFreq); // gui::freqSelect.frequency);
                    gui::waterfall.centerFreqMoved = true;
                    gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);

                    // int tuningMode = gui::mainWindow.gettuningMode();
                    // tuner::tune(tuningMode, gui::waterfall.selectedVFO, CentralFreq);

                    AddSelectedList_Short();

                    // gui::waterfall.VFOMoveSingleClick = false;
                    core::configManager.acquire();
                    core::configManager.conf["centerTuning"] = false;
                    core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                    core::configManager.release(true);
                }
                initial_find_level = true;
                core::modComManager.callInterface("Запис", MAIN_SET_STATUS_CHANGE, NULL, NULL);

                {
                    double bw = 0.922;
                    gui::mainWindow.setViewBandwidthSlider(0.922);
                    double factor = bw * bw;
                    if (factor > 0.85)
                        factor = 0.85;
                    double wfBw = gui::waterfall.getBandwidth();
                    double delta = wfBw - 1000.0;
                    double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);
                    // flog::info("bw4 {0}, finalBw {1}, wfBw {2}, delta {3}, factor {4}", bw, finalBw, wfBw, delta, factor);
                    if (finalBw > VIEWBANDWICH)
                        finalBw = VIEWBANDWICH;
                    gui::waterfall.setViewBandwidth(finalBw);
                    gui::waterfall.setViewOffset(gui::waterfall.vfos["Канал приймання"]->centerOffset); // center vfo on screen
                }

                // flog::info("[supervision4] Restarting player with new VFO configuration...");
                // gui::mainWindow.setPlayState(true);

                refreshWaterfallBookmarks(true);

                gui::mainWindow.setbutton_ctrl(gui::mainWindow.getCurrServer(), true);
                gui::mainWindow.setidOfList_ctrl(gui::mainWindow.getCurrServer(), selectedListId);
                gui::mainWindow.setUpdateMenuSnd7Ctrl(gui::mainWindow.getCurrServer(), true);

                int _air_recording;
                core::modComManager.callInterface("Airspy", AIRSPY_IFACE_CMD_GET_RECORDING, NULL, &_air_recording);
                // flog::info("AIR Recording is '{0}'", _air_recording);
                // flog::info("start() Control4(), running={0},  CentralFreq {1}", running.load(), CentralFreq);
                if (_air_recording == 1)
                {
                    // running.store(true);
                    workerThread = std::thread(&SupervisorModeModule::worker, this);
                }
                core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);

                itbook = bookmarks.begin();
                _recording = false;
                core::configManager.acquire();
                try
                {
                    radioMode = (int)core::configManager.conf["RadioMode"];
                }
                catch (const std::exception &e)
                {
                    radioMode = 0;
                }
                core::configManager.release();

                if (radioMode == 0)
                {
                    status_direction = true;
                }
                else
                {
                    status_direction = true; // false;
                }

                for (int i = 0; i < maxCHANNELS; i++)
                    ch_recording[i] = false;

                
                // И только теперь переводим модуль в рабочее состояние
                currentState.store(ModuleState::RUNNING);
                gui::mainWindow.setStopMenuUI(false);
                flog::info("[SUPERVISOR] Module is now RUNNING.");                
            }).detach(); // Отсоединяем, так как GUI не должен ждать завершения

        }
    }

    void stop()
    {
        if (isARM)
            return;
        flog::info("[SUPERVISOR] STOP");
        // Атомарная операция: пытаемся перевести состояние из RUNNING в STOPPING
        ModuleState expected = ModuleState::RUNNING;
        if (currentState.compare_exchange_strong(expected, ModuleState::STOPPING))
        {
            // Если получилось, мы инициировали остановку.
            flog::info("[SUPERVISOR] Stop sequence requested...");

            // std::unique_lock<std::mutex> lock(stopMtx, std::try_to_lock);
            // if (!running.load())
            // {
            //    flog::warn("Stop called but already stopped. Exiting.");
            //    return;
            // }

            // 1. Посылаем сигнал нашему собственному worker'у, чтобы он перестал сканировать
            // running.store(false);
            std::thread([this]()
                        {
                // --- Этап 3: Ждем завершения основного рабочего потока ---
                flog::info("[Cleanup Thread] Joining workerThread...");
                if (this->workerThread.joinable())
                    this->workerThread.join();
                flog::info("[Cleanup Thread] workerThread joined.");

                // --- Этап 1: Рассылаем команду STOP всем рекордерам ---
                flog::info("[Cleanup Thread] Sending STOP command to recorders...");
                if (_recording)
                {
                    int i = 0;
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (ch_recording[i])
                        {
                            std::string rec_name = "Запис " + name;
                            core::modComManager.callInterface(rec_name.c_str(), RECORDER_IFACE_CMD_STOP, NULL, NULL);
                        }
                        ch_recording[i] = false;
                        i++;
                    }
                }

                // --- Этап 2:  БЛОК ОЧИСТКИ (выполняется ТОЛЬКО ПОСЛЕ выхода из цикла)
                flog::info("[Cleanup Thread] Waiting for all recorders to confirm they have stopped...");
                bool all_stopped;
                int retries = 50; // Ждем максимум 5 секунд
                do
                {
                    all_stopped = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    for (auto &[name, bm] : bookmarks)
                    {
                        std::string rec_name = "Запис " + name;
                        if (core::moduleManager.instanceExist(rec_name))
                        {
                            // RECORDER_IFACE_GET_RECORD возвращает 'recording', который становится false
                            // в самом конце _internal_stop.
                            bool is_rec_running = true;
                            core::modComManager.callInterface(rec_name.c_str(), RECORDER_IFACE_GET_RECORD, NULL, &is_rec_running);
                            if (is_rec_running)
                            {
                                all_stopped = false;
                                break;
                            }
                        }
                    }
                } while (!all_stopped && --retries > 0);

                // --- Этап 4: Удаляем инстансы ---
                flog::info("[Cleanup Thread] Deleting instances via DelSelectedList...");
                this->DelSelectedList();
                // --- Этап 5: Финальная очистка состояния ---
                this->_recording = false;
                core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);
                gui::mainWindow.setbutton_ctrl(this->CurrSrvr, false);
                gui::mainWindow.setUpdateMenuSnd7Ctrl(this->CurrSrvr, true);
                flog::info("[Cleanup Thread] Cleanup sequence finished.");
                currentState.store(ModuleState::STOPPED); })
                .detach();
        }
        else
        {
            flog::warn("[SUPERVISOR] Stop called but module is not in RUNNING state.");
        } // 4. Сбрасываем флаг, когда все действительно закончено.
          // is_stopping.store(false);
          // ВСЁ. Функция stop() завершается. Она не вызывает join() и DelSelectedList.
          // GUI не виснет.
    }

    // Модифицированная функция для получения максимального уровня и оценки шума
    struct LevelInfo
    {
        float maxLevel;   // Максимальный уровень (сигнал)
        float noiseLevel; // Оценка шума (среднее значение нижних уровней)
    };

    LevelInfo getMaxLevelNew(float *data, double freq, double width, int dataWidth, double wfStart, double wfWidth)
    {
        if (!data || dataWidth <= 0 || wfWidth <= 0)
        {
            return {-150.0f, -150.0f};
        }

        double halfWidth = width / 2.0;
        double low = freq - halfWidth;
        double high = freq + halfWidth;

        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);

        float max = -INFINITY;
        std::vector<float> levels; // Для хранения всех значений в поддиапазоне
        for (int i = lowId; i <= highId; i++)
        {
            if (data[i] > max)
            {
                max = data[i];
            }
            levels.push_back(data[i]);
        }

        if (max == -INFINITY || max < -150.0f)
        {
            max = -150.0f;
        }

        // Оценка шума: среднее значение нижних 50% уровней
        float noiseLevel = -150.0f;
        if (!levels.empty())
        {
            std::sort(levels.begin(), levels.end());
            int mid = levels.size() / 2;
            noiseLevel = std::accumulate(levels.begin(), levels.begin() + mid, 0.0f) / mid;
            if (noiseLevel < -150.0f)
                noiseLevel = -150.0f;
        }

        return {max, noiseLevel};
    }

    // Функция сканирования диапазона, возвращающая signalThreshold
    float scanRange(int numSegments, float *data, int dataWidth, double wfStart, double wfWidth, double threshold = 8.0)
    {
        const double scan_band = 12500.0;
        std::vector<double> noiseLevels; // Для хранения уровней шума
        double minSignalLevel = std::numeric_limits<double>::max();

        for (int i = 0; i < numSegments; ++i)
        {
            double startFreq = sigmentLeft + i * scan_band;
            double endFreq = startFreq + scan_band;
            double centerFreq = (startFreq + endFreq) / 2.0;

            // Получаем максимальный уровень и оценку шума
            LevelInfo levels = getMaxLevelNew(data, centerFreq, scan_band, dataWidth, wfStart, wfWidth);
            double signalLevel = static_cast<double>(levels.maxLevel);
            double noiseLevel = static_cast<double>(levels.noiseLevel);

            // Если разница между максимальным уровнем и шумом мала (например, < 10 дБ), считаем это шумом
            if (signalLevel - noiseLevel < 10.0)
            {
                noiseLevels.push_back(noiseLevel);
            }
            else
            {
                // Если есть сильный сигнал, не учитываем этот поддиапазон для шума
                noiseLevels.push_back(-150.0); // Минимальный уровень как заглушка
            }

            // Обновляем минимальный уровень сигнала (для информации)
            if (signalLevel < minSignalLevel)
            {
                minSignalLevel = signalLevel;
            }

            // std::cout << "Поддиапазон " << startFreq << " - " << endFreq
            //          << ": Уровень сигнала = " << signalLevel << " дБ, Уровень шума = " << noiseLevel << " дБ" << std::endl;
        }

        // Вычисление среднего уровня шума (игнорируем поддиапазоны с сильным сигналом)
        double avgNoiseLevel = 0.0;
        int validNoiseCount = 0;
        for (double level : noiseLevels)
        {
            if (level > -150.0)
            { // Учитываем только поддиапазоны с реальным шумом
                avgNoiseLevel += level;
                validNoiseCount++;
            }
        }
        if (validNoiseCount > 0)
        {
            avgNoiseLevel /= validNoiseCount;
        }
        else
        {
            avgNoiseLevel = -150.0; // Если нет данных о шуме
        }

        // Порог для сигнала
        double signalThreshold = avgNoiseLevel + threshold;

        // Вывод результатов
        flog::warn("Порог для отличия сигнала от шума: {0} дБ (шум + {1} дБ), minSignalLevel {2}, avgNoiseLevel {3}", signalThreshold, threshold, minSignalLevel, avgNoiseLevel);
        // std::cout << "\nМинимальный уровень сигнала: " << minSignalLevel << " дБ" << std::endl;
        // std::cout << "Средний уровень шума (без учета сигналов): " << avgNoiseLevel << " дБ" << std::endl;
        // std::cout << "Порог для отличия сигнала от шума: " << signalThreshold << " дБ (шум + " << threshold << " дБ)" << std::endl;

        return static_cast<float>(signalThreshold);
    }

    void handleWaitingForAKF(const int ch_num, std::string RECORD_CH_Name, const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        // Вместо использования сложных таймеров, мы просто запрашиваем
        // два ключевых параметра у модуля Recorder.
        bool is_recorder_still_recording = false;
        int signal_result_from_recorder = -1; // -1: в ожидании, 0: шум, >0: сигнал

        core::modComManager.callInterface(RECORD_CH_Name.c_str(), RECORDER_IFACE_GET_RECORD, NULL, &is_recorder_still_recording);
        core::modComManager.callInterface(RECORD_CH_Name.c_str(), RECORDER_IFACE_GET_SIGNAL, NULL, &signal_result_from_recorder);

        // ==========================================================
        //                        Логика решений
        // ==========================================================

        // СЦЕНАРИЙ 1: Рекордер сообщил, что нашел СИГНАЛ.
        // Это наш успешный случай. Мы переходим в режим мониторинга.
        if (signal_result_from_recorder > 0)
        {
            std::string signal_type = (signal_result_from_recorder == 2) ? "DMR" : "VOICE/OTHER";
            flog::info("AKF result for {0}: SUCCESS ({1}). Recorder continues recording, supervisor will monitor.", RECORD_CH_Name, signal_type);

            // Рекордер сам продолжит запись, нам ничего останавливать не нужно.
            // Мы просто меняем свое состояние на 'RECEIVING', чтобы worker перешел
            // к следующему каналу для сканирования, оставив этот в покое.
            channel_states[ch_num].store(State::RECEIVING);
            return;
        }

        // СЦЕНАРИЙ 2: Рекордер УЖЕ ОСТАНОВИЛ запись.
        // Это значит, что внутренний таймер рекордера сработал, он решил,
        // что это ШУМ/ТАЙМАУТ, и сам себя остановил. Мы просто принимаем это.
        if (!is_recorder_still_recording && signal_result_from_recorder <= 0)
        {
            flog::info("AKF result for {0}: NOISE/TIMEOUT. Recorder has stopped. Supervisor will resume scanning.", RECORD_CH_Name);

            // Рекордер уже остановлен. Мы просто меняем свое состояние,
            // чтобы worker продолжил сканирование.
            channel_states[ch_num].store(State::RECEIVING);
            return;
        }

        // СЦЕНАРИЙ 3: Рекордер все еще пишет, и результат еще не известен.
        // (is_recorder_still_recording == true && signal_result_from_recorder == -1)
        // Это значит, что рекордер все еще ждет ответа от UDP.
        // Мы НИЧЕГО НЕ ДЕЛАЕМ. Просто ждем следующей итерации цикла worker,
        // чтобы снова проверить состояние.
        // flog::info("Waiting for AKF decision from {0}...", RECORD_CH_Name);

        // ==========================================================
        //           Дополнительная защита от "вечного зависания"
        // ==========================================================
        int time_spent_waiting_ms = (std::chrono::duration_cast<std::chrono::milliseconds>(now - ch_StartTime[ch_num])).count();

        // Если мы ждем неоправданно долго (например, 10 секунд), значит,
        // что-то пошло не так в рекордере. Принудительно останавливаем его.
        if (time_spent_waiting_ms > 8000)
        {
            flog::error("Supervisor safety timeout for {0}! Waited more than 8 seconds. Forcing stop.", RECORD_CH_Name);
            core::modComManager.callInterface(RECORD_CH_Name.c_str(), RECORDER_IFACE_CMD_STOP, NULL, NULL);

            // Меняем состояние, чтобы выйти из ожидания и продолжить сканирование.
            channel_states[ch_num].store(State::RECEIVING);
        }
    }

    void worker()
    {
        // 10Hz scan loop
        bool _Receiving = true;
        int _mode;
        int recMode = 1; // RECORDER_MODE_AUDIO;
        tuning = false;
        _detected = false;

        auto init_level_time = std::chrono::high_resolution_clock::now(); // std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::seconds(1));

        if (itbook != bookmarks.end())
        {
            name = itbook->first;
            ch_num = 0;
            auto bm = itbook->second;
            current = bm.frequency;
            curr_bw = bm.bandwidth;
            ch_rec_Name = "Запис " + name;
            ch_curr_selectedVFO = name;
            // curr_level = bm.level;
            if (getflag_level[CurrSrvr] == true)
                curr_level = getgen_level[CurrSrvr];
            else
                curr_level = bm.level;
            ch_curr_Signal = bm.Signal;
        }
        else
            return;

        bool _r = false;
        for (size_t i = 0; i < maxCHANNELS; i++)
        {
            if (ch_recording[i] == true)
            {
                _r = true;
                // flog::info("ch_recording[{0}]={1}, MAX_ch_num={2}, _recording = {3}", i, ch_recording[i], maxCHANNELS, _recording);
            }
        }

        // int update_level = 0;

        std::map<std::string, ObservationBookmark>::iterator itbook;
        bool iterator_is_valid = false;
        // ===================================
        //            ОСНОВНОЙ ЦИКЛ РАБОТЫ
        // ===================================
        while (currentState.load() == ModuleState::RUNNING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scan3Mtx);
                auto now = std::chrono::high_resolution_clock::now();

                bool needs_reset = bookmarksUpdated.load() || !iterator_is_valid;
                if (needs_reset)
                {
                    // Логируем сброс только если причина - обновление от другого потока
                    if (bookmarksUpdated.load())
                    {
                        flog::info("[Worker] Bookmarks were updated by another thread. Resetting iterator.");
                    }
                    itbook = bookmarks.begin();
                    bookmarksUpdated.store(false);
                }

                if (bookmarks.empty())
                {
                    iterator_is_valid = false; // Итератор больше не валиден, т.к. список пуст.
                    continue;                  // Пропускаем итерацию, на следующей снова проверим.
                }
                if (itbook == bookmarks.end())
                {
                    itbook = bookmarks.begin();
                    MAX_ch_num = ch_num; // Фиксируем кол-во каналов
                    ch_num = 0;
                    if (itbook == bookmarks.end())
                    {
                        iterator_is_valid = false;
                        continue;
                    }
                }
                iterator_is_valid = true; // Если мы дошли до сюда, итератор точно валиден.

                // --- Блок обновления рабочих переменных ---// Теперь, когда мы на 100% уверены, что itbook валиден, работаем с ним.
                name = itbook->first;
                auto &bm = itbook->second;
                current = bm.frequency;
                name = itbook->first;
                curr_bw = bm.bandwidth;
                ch_rec_Name = "Запис " + name;
                ch_curr_selectedVFO = name;
                ch_curr_Signal = bm.Signal;
                if (getflag_level[CurrSrvr] || status_auto_level4)
                    curr_level = getgen_level[CurrSrvr];
                else
                    curr_level = bm.level;

                // Get FFT data
                int dataWidth = 0;
                float *data = gui::waterfall.acquireLatestFFT(dataWidth);
                if (!data)
                {
                    continue;
                }
                // Get gather waterfall data
                double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency(); // current
                double wfWidth = gui::waterfall.getViewBandwidth();                                     // curr_bw; //
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);

                // Gather VFO data
                //====================================================
                if (status_auto_level4 && initial_find_level)
                {
                    int samplesPerSegment = 1000; // Количество отсчетов на поддиапазон
                    int numSegments = static_cast<int>((sigmentRight - sigmentLeft) / scan_band);

                    // Сканирование диапазона и поиск минимального уровня сигнала
                    float signalThreshold = scanRange(numSegments, data, dataWidth, wfStart, wfWidth);
                    curr_level = static_cast<int>(signalThreshold);
                    getgen_level[CurrSrvr] = curr_level;
                    gui::mainWindow.setLevelDbCtrl(CurrSrvr, curr_level);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(CurrSrvr, true);
                    // Вывод результатов
                    // std::cout << "Возвращенный порог сигнала: " << signalThreshold << " дБ" << std::endl;
                    initial_find_level = false;
                    gui::mainWindow.setChangeGainFalse();
                    init_level_time = std::chrono::high_resolution_clock::now();
                }
                //====================================================
                // Check if we are waiting for a tune
                if (_Receiving)
                {
                    double vfoWidth = sigpath::vfoManager.getBandwidth(ch_curr_selectedVFO);
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    /*
                    if (status_auto_level4 && ch_recording[ch_num] == false) {
                        if (update_level == 10) {
                            double current_left = current + vfoWidth + vfoWidth / 2;
                            double current_rght = current - (vfoWidth + vfoWidth / 2);
                            float maxLevel_left = getMaxLevel(data, current_left, vfoWidth, dataWidth, wfStart, wfWidth);
                            float maxLevel_rght = getMaxLevel(data, current_rght, vfoWidth, dataWidth, wfStart, wfWidth);
                            float Averaging = (maxLevel_left + maxLevel_rght) / 2;
                            int iLevel = round(Averaging + 10.0);

                            flog::info(" >> current = {0}, maxLevel {1},  maxLevel_left {2}, maxLevel_rght {3}, curr_level {4}, new_level {5}  ", current, maxLevel, maxLevel_left, maxLevel_rght, curr_level, iLevel);

                            int diff_level = iLevel - curr_level;
                            if (diff_level < -7 || diff_level > 7) {
                                flog::info(" Update LEVELS! Old Level {0}. New Level = {1}. iLevel > level+5 ({2}) && iLevel < level-5 ({3})!", curr_level, iLevel, curr_level + 5, curr_level - 5);
                                genlevel = iLevel;
                                curr_level = genlevel;
                                gui::mainWindow.setLevelDbCtrl(CurrSrvr, curr_level);
                                gui::mainWindow.setUpdateMenuSnd7Ctrl(CurrSrvr, true);
                            }
                            update_level = 0;
                        }
                        else
                            update_level++;
                    }
                    */
                    if (maxLevel > curr_level)
                    {
                        // flog::info(" >> current={0}, _Receiving==true, maxLevel = {1} > curr_level {2}, vfoWidth {3} ", current, maxLevel, curr_level, vfoWidth);
                        // flog::info(" >> wfCenter={0}, wfWidth = {1}, wfStart {2}, wfEnd {3}, getViewOffset {4}, ViewBandwidth {5}, CFrequency {6}!", wfCenter, wfWidth, wfStart, wfEnd, gui::waterfall.getViewOffset(), gui::waterfall.getViewBandwidth(), gui::waterfall.getCenterFrequency());
                         ModuleState actual_state = currentState.load();
                        if (ch_recording[ch_num] == false && actual_state == ModuleState::RUNNING)
                        {
                            flog::info("TRACE. START Receiving! curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);
                            if (gui::waterfall.selectedVFO != "")
                            {
                                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio")
                                {

                                    _mode = gui::mainWindow.getselectedDemodID();
                                }
                                else
                                {
                                    try
                                    {
                                        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                                    }
                                    catch (const std::exception &e)
                                    {
                                        flog::warn("[TRACE] callInterface fallback (radioMode). Error: {0}", e.what());
                                    }
                                }
                            }
                            std::string templ = "$y$M$d-$u-$f-$b-$n-$e.wav";
                            core::configManager.acquire();
                            try
                            {
                                maxRecDuration = core::configManager.conf["maxRecDuration"];
                            }
                            catch (...)
                            {
                            }
                            core::configManager.release();
                            curr_nameWavFile[ch_num] = genWavFileName(templ, current, _mode);
                            if (curr_nameWavFile[ch_num] != "")
                                curlPOST_begin(curr_nameWavFile[ch_num]);
                            flog::info(" << START Recording {0} ({1}), curr_level = {2}, maxLevel = {3}, maxRecWaitTime {4} !", ch_rec_Name, ch_num, curr_level, maxLevel, maxRecWaitTime);
                            int recMode = 1; // RECORDER_MODE_AUDIO;
                            core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);

                            int typeSrch = status_AKF ? (ch_curr_Signal + 1) : 0;
                            flog::info(" typeSrch {0}, typeSrch");
                            if (typeSrch > 0)
                            {
                                channel_states[ch_num].store(State::WAITING_FOR_AKF);
                                ADD_WAIT_MS[ch_num] = 0;
                            }
                            else
                                channel_states[ch_num].store(State::RECEIVING);

                            core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START_AKF, &typeSrch, NULL);

                            core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START, (void *)curr_nameWavFile[ch_num].c_str(), NULL);

                            ch_StartTime[ch_num] = now;
                            ch_recording[ch_num] = true;

                            _recording = true;
                        }
                        else
                        {
                            if (channel_states[ch_num].load() == State::WAITING_FOR_AKF)
                            {
                                handleWaitingForAKF(ch_num, ch_rec_Name, now);
                            }
                            // flog::info("TRACE. {0} ({1}), curr_level = {2}, maxLevel = {3}, ch_recording[ch_num] {4} !", ch_rec_Name, ch_num, curr_level, maxLevel, ch_recording[ch_num]);
                            if (channel_states[ch_num].load() == State::STOPPING || (channel_states[ch_num].load() == State::RECEIVING && (std::chrono::duration_cast<std::chrono::milliseconds>(now - ch_StartTime[ch_num])).count() > maxRecDuration * 60000))
                            {
                                // RESTART
                                channel_states[ch_num].store(State::STOPPED);
                                core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                                if (curr_nameWavFile[ch_num] != "") //  && radioMode == 0
                                    curlPOST_end(curr_nameWavFile[ch_num]);

                                std::string templ = "$y$M$d-$u-$f-$b-$n-$e.wav";
                                curr_nameWavFile[ch_num] = genWavFileName(templ, current, _mode);
                                core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                                flog::info("    RESTART Receiving {0} ({1}) !", ch_rec_Name, ch_num);

                                int typeSrch = status_AKF ? (ch_curr_Signal + 1) : 0;
                                core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START_AKF, &typeSrch, NULL);
                                recMode = 1;
                                core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                                core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START, (void *)curr_nameWavFile[ch_num].c_str(), NULL);
                                if (curr_nameWavFile[ch_num] != "") //  && radioMode == 0
                                    curlPOST_begin(curr_nameWavFile[ch_num]);

                                ch_StartTime[ch_num] = now;
                                ch_recording[ch_num] = true;
                                _recording = true;
                            }
                        }

                        // flog::info("TRACE. START Receiving... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _record={5} !", curr_level, maxLevel, current, _lingerTime, _recording, _record);
                        ch_lastSignalTime[ch_num] = now;
                        _Receiving = false;
                        _detected = true;
                    }
                    else
                    {
                        if (_detected == false)
                        {
                            // flog::info("_detected {0} ch_num {1} ch_recording[ch_num] {2}, maxRecWaitTime {3} !", _detected, ch_num, ch_recording[ch_num], maxRecWaitTime);
                            int wait_time_ms = maxRecWaitTime * 1000; // Значение по умолчанию

                            // Если включен АКФ, получаем реальное время ожидания от рекордера
                            if (status_AKF)
                            {
                                int akf_timeout_from_recorder = 0;
                                // ch_rec_Name - это имя инстанса рекордера, например "Запис C3"
                                core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_GET_AKF_TIMEOUT, NULL, &akf_timeout_from_recorder);

                                // Если рекордер вернул валидный таймаут, используем его
                                if (akf_timeout_from_recorder > 0)
                                {
                                    // Добавляем небольшой запас (например, 500 мс) на случай сетевых задержек
                                    wait_time_ms = (akf_timeout_from_recorder * 1000) + 500;
                                }
                            }
                            if (ch_recording[ch_num] == true)
                            {
                                if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - ch_lastSignalTime[ch_num])).count() > wait_time_ms)
                                {
                                    core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                                    flog::info("    STOP Receiving {0} ({1}) !", ch_rec_Name, ch_num);

                                    ch_recording[ch_num] = false;
                                    if (curr_nameWavFile[ch_num] != "")
                                    {
                                        flog::info(" >> STOP Receiving 2 {0}! {1} - selectedVFO, curr_level = {2}, maxLevel = {3}, _recording={4}, _detected={5} !", ch_num, ch_curr_selectedVFO, curr_level, maxLevel, ch_recording[ch_num], _detected);
                                        curlPOST_end(curr_nameWavFile[ch_num]);
                                        curr_nameWavFile[ch_num] = "";
                                    }
                                }
                            }
                        }
                        _Receiving = false;
                    }
                }

                if (_Receiving == false)
                {
                    // flog::info(" ! ch_curr_selectedVFO {0}", ch_curr_selectedVFO);
                    // if (correct_level==0) {
                    // itbook = next(itbook);
                    itbook++; // Безопасно переходим к следующему элементу
                    ch_num++;
                    /*
                    if (itbook == bookmarks.end())
                    {
                        itbook = bookmarks.begin();
                        MAX_ch_num = ch_num;
                        ch_num = 0;
                    }
                    */
                    // }
                    // next_bank++;
                    int _r = false;
                    for (size_t i = 0; i < MAX_ch_num; i++)
                    {
                        if (ch_recording[i] == true)
                        {
                            _r = true;
                            // flog::info("ch_recording[{0}]={1}, MAX_ch_num={2}, _recording = {3}", i, ch_recording[i], MAX_ch_num, _recording);
                        }
                    }
                    _recording = _r;
                    if (!gui::waterfall.setCurrVFO(ch_curr_selectedVFO))
                    {
                        _Receiving = false;
                        if (ch_recording[ch_num] == true)
                            core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                        ch_recording[ch_num] = false;
                        //    _skip = true;
                    }
                    else
                    {
                        _Receiving = true;
                    }

                    _detected = false;
                }

                //====================================================
                if (status_auto_level4)
                {
                    /*
                    bool new_gain = gui::mainWindow.getChangeGain();
                    if (new_gain) {
                        flog::info("new_gain {0}", new_gain);
                        gui::mainWindow.setChangeGainFalse();
                        trigger_time = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                        timer_started = true;
                    }
                    if (timer_started && std::chrono::steady_clock::now() >= trigger_time) {
                        initial_find_level = true;
                        timer_started = false;
                    }
                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() > INTERVAL_FOR_FIND_Threshold * 60000) {
                        flog::info("time = {0}, (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() {1} ", (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count(), INTERVAL_FOR_FIND_Threshold * 60000);
                        initial_find_level = true;
                    }
                    */

                    if (cnt_skip == COUNT_FOR_REFIND_SKIP)
                    {
                        initial_find_level = true;
                        flog::info("REFIND_LEVEL cnt_skip ={0}", cnt_skip);
                    }

                    bool new_gain = gui::mainWindow.getChangeGain();
                    if (new_gain)
                    {
                        flog::info("new_gain {0}", new_gain);
                        gui::mainWindow.setChangeGainFalse();
                        trigger_time = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                        timer_started = true;
                    }
                    if (timer_started && std::chrono::steady_clock::now() >= trigger_time)
                    {
                        initial_find_level = true;
                        timer_started = false;
                    }

                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() > INTERVAL_FOR_FIND_Threshold * 60000)
                    {
                        flog::info("time = {0}, (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() {1} ", (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count(), INTERVAL_FOR_FIND_Threshold * 60000);
                        initial_find_level = true;
                    }
                }
                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
            }
        }
        // flog::info("record = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);
    }

    struct Drawn
    {
        ImRect rect;
        int index;
    };

    std::vector<Drawn> rects;

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void *ctx)
    {
        SupervisorModeModule *_this = (SupervisorModeModule *)ctx;
        _this->rects.clear();
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF)
        {
            return;
        }

        if (!_this->infoThreadStarted.load())
        {
            if (_this->isServer || _this->isARM)
            {
                _this->infoThreadStarted.store(true); // Флаг для безопасной остановки в деструкторе
                _this->workerInfoThread = std::thread(&SupervisorModeModule::workerInfo, _this);
            }
        }
        int index = -1;
        auto ctm = currentTimeMillis();
        for (auto const &bm : _this->waterfallBookmarks)
        {
            index++;

            if (bm.notValidAfter && ctm > bm.notValidAfter)
            {
                continue;
            }

            double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

            ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
            ImVec2 rectMin;
            float layoutOverlapStep = nameSize.y + 1;
            if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP)
            {
                rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
            }
            else
            {
                rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                layoutOverlapStep = -layoutOverlapStep;
            }

            ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, rectMin.y + nameSize.y);
            ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
            ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

            if (clampedRectMax.x - clampedRectMin.x > 0)
            {
                auto newRect = ImRect{clampedRectMin, clampedRectMax};
            again:
                for (const auto &existing : _this->rects)
                {
                    if (existing.rect.Overlaps(newRect))
                    {
                        newRect.Min.y += layoutOverlapStep;
                        newRect.Max.y += layoutOverlapStep;
                        goto again;
                    }
                }
                clampedRectMax = newRect.Max;
                clampedRectMin = newRect.Min;
                rectMin.y = clampedRectMin.y;
                rectMax.y = clampedRectMax.y;
                if (clampedRectMin.y < args.min.y || clampedRectMax.y >= args.max.y)
                {
                    continue; // dont draw at all.
                }
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, bm.worked ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 0, 255));
                _this->rects.emplace_back(Drawn{newRect, index});
            }
            if (rectMin.x >= args.min.x && rectMax.x <= args.max.x)
            {
                args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), rectMin.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
            }
            if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq)
            {
                args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), bm.worked ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 0, 255));
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void *ctx)
    {
        SupervisorModeModule *_this = (SupervisorModeModule *)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF)
        {
            return;
        }

        if (_this->mouseClickedInLabel)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        // First check that the mouse clicked outside of any label. Also get the bookmark that's hovered
        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP)
        {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--)
            {
                auto &bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMin.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMin.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax))
                {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM)
        {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--)
            {
                auto &bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMax.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMax.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax))
                {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel)
        {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel)
        {
            return;
        }

        gui::waterfall.inputHandled = true;

        double centerXpos = args.fftRectMin.x + std::round((hoveredBookmark.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
        ImVec2 nameSize = ImGui::CalcTextSize(hoveredBookmarkName.c_str());
        ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? (args.fftRectMax.y - nameSize.y) : args.fftRectMin.y);
        ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? args.fftRectMax.y : args.fftRectMin.y + nameSize.y);
        ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
        ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            _this->mouseClickedInLabel = true;
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("Банк: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Назва: %s", hoveredBookmark.bookmark.dopinfo.c_str());
        ImGui::Text("Частота: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Ширина Смуги: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Вид демод.: %s", demodModeList[hoveredBookmark.bookmark.mode]);
        // ImGui::Text("Поріг виявлення: %s", utils::formatFreq(hoveredBookmark.bookmark.level).c_str());
        ImGui::Text("Тип сигналу: %s", std::to_string(hoveredBookmark.bookmark.Signal).c_str());
        ImGui::Text("Звукова: %s", hoveredBookmark.bookmark.scard.c_str());

        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;

    pfd::open_file *importDialog;
    pfd::save_file *exportDialog;

    bool importBookmarks(std::string path)
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
        if (!importBookmarks.is_object())
        {
            flog::error("Файл JSON должен быть объектом");
            return false;
        }

        // std::ifstream fs(path);
        // json importBookmarks;
        // fs >> importBookmarks;
        // "rx-mode":"observation"
        if (!importBookmarks["rx-mode"].is_string())
        {
            flog::error("Bookmark attribute is invalid ('rx-mode' error");
        }

        if (importBookmarks["rx-mode"] != "observation")
        {
            flog::error("Bookmark attribute is invalid ('rx-mode' must have the name 'observation')");
            return false;
        }

        std::string NameList = "";
        for (auto const [_name, bm] : importBookmarks.items())
        {
            flog::warn("NameList _name'{0}'", _name);
            if (_name != "InstNum" && _name != "bank-name" && _name != "domain" && _name != "rx-mode" && _name != "time_created")
            {
                NameList = std::string(_name);
                break;
            }
        }
        flog::info("NameList = {0} ", NameList);
        if (!importBookmarks[NameList].contains("bookmarks"))
        {
            flog::error("File does not contains any bookmarks");
            return false;
        }

        if (!importBookmarks[NameList]["bookmarks"].is_object())
        {
            flog::error("Bookmark attribute is invalid");
            return false;
        }

        json newList = json({});
        config.acquire();
        newList["showOnWaterfall"] = true;

        newList["flaglevel"] = false;
        newList["genlevel"] = -70;

        newList["bookmarks"] = json::object();
        config.conf["lists"][NameList] = newList;
        config.conf["selectedList"] = NameList;
        config.release(true);

        refreshLists();
        selectedListName = NameList;
        loadByName(selectedListName);
        // Load every bookmark
        for (auto const [_name, bm] : importBookmarks[NameList]["bookmarks"].items())
        {
            if (_name[0] != 'C')
            {
                flog::error("Bookmark with the name '{0}' not correct, stopping", _name);
                continue;
            }
            if (bookmarks.find(_name) != bookmarks.end())
            {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.scard = bm["scard"];
            fbm.level = bm["level"];
            try
            {
                fbm.dopinfo = bm["dopinfo"];
            }
            catch (const std::exception &e)
            {
                fbm.dopinfo = _name;
            }
            try
            {
                fbm.Signal = bm["Signal"];
            }
            catch (const std::exception &e)
            {
                fbm.Signal = 0;
            }
            fbm.selected = false;
            bookmarks[_name] = fbm;
        }

        saveByName(selectedListName);
        fs.close();
        return true;
    }

    void exportBookmarks(std::string path)
    {
        std::ofstream fs(path);
        fs << exportedBookmarks;
        fs.close();
    }

    std::string expandString(std::string input)
    {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    static void moduleInterfaceHandler(int freq, void *in, void *out, void *ctx)
    {
        SupervisorModeModule *_this = (SupervisorModeModule *)ctx;
        flog::info("moduleInterfaceHandler, name = {0} ", _this->selectedListName);

        struct FreqData
        {
            int freq;
            int mode;
            int Signal;
        } pFreqData;
        pFreqData = *(static_cast<FreqData *>(in));
        // pFreqData.freq = _this->current;
        int _mode = pFreqData.mode; //  *(int*)in;
        if (gui::waterfall.selectedVFO == "")
        {
            _this->editedBookmark.frequency = freq;
            _this->editedBookmark.bandwidth = 0;
            _this->editedBookmark.mode = (int)_mode;
        }
        else
        {
            _this->editedBookmark.frequency = freq;
            _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            _this->editedBookmark.mode = (int)_mode;
            /*
            if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                int mode=1;
             //   core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                _this->editedBookmark.mode = mode;
            }
            */
        }
        // _this->devListId = 0;
        // _this->editedBookmark.scard = _this->currVCardDevList[_this->devListId];

        _this->editedBookmark.level = _this->getgen_level[_this->CurrSrvr];
        _this->editedBookmark.Signal = pFreqData.Signal;

        _this->editedBookmark.selected = false;

        _this->createOpen = true;

        // Find new unique default name
        if (_this->bookmarks.find("Новий") == _this->bookmarks.end())
        {
            _this->editedBookmarkName = "Новий";
        }
        else
        {
            char buf[64];
            for (int i = 1; i < 1000; i++)
            {
                sprintf(buf, "Новий (%d)", i);
                if (_this->bookmarks.find(buf) == _this->bookmarks.end())
                {
                    break;
                }
            }
            _this->editedBookmarkName = buf;
        }

        // If editing, delete the original one
        //               if (editOpen) {
        _this->bookmarks.erase(_this->firstEditedBookmarkName);
        //               }
        _this->bookmarks[_this->editedBookmarkName] = _this->editedBookmark;

        _this->saveByName(_this->selectedListName);
    }

    float getMaxLevelOld(float *data, double freq, double width, int dataWidth, double wfStart, double wfWidth)
    {
        width = width / 2;
        double low = freq - (width / 2.0);
        double high = freq + (width / 2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++)
        {
            if (data[i] > max)
            {
                max = data[i];
            }
        }
        if (max < -150)
            max = -150;
        return max;
    }
    // Скорректированная функция getMaxLevel
    /*
    float getMaxLevel(float* data, double freq, double width, int dataWidth, double wfStart, double wfWidth) {
        if (!data || dataWidth <= 0 || wfWidth <= 0) {
            return -150.0f;
        }

        double halfWidth = width / 2.0;
        double low = freq - halfWidth;
        double high = freq + halfWidth;

        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);

        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (data[i] > max) {
                max = data[i];
            }
        }

        if (max == -INFINITY || max < -150.0f) {
            max = -150.0f;
        }

        return max;
    }
    */
    float getMaxLevel(float *data, double freq, double width, int dataWidth, double wfStart, double wfWidth)
    {
        // Проверка на корректность входных данных
        if (!data || dataWidth <= 0 || width <= 0.0 || wfWidth <= 0.0)
            return -150.0f;

        // Вычисляем границы интересующей полосы частот
        double halfWidth = width / 2.0;
        double lowFreq = freq - halfWidth;
        double highFreq = freq + halfWidth;

        // Преобразуем частоты в индексы массива с округлением
        int lowId = std::clamp<int>(std::round((lowFreq - wfStart) * dataWidth / wfWidth), 0, dataWidth - 1);
        int highId = std::clamp<int>(std::round((highFreq - wfStart) * dataWidth / wfWidth), 0, dataWidth - 1);

        // Защита от ситуации, когда индексы перепутаны
        if (lowId > highId)
            std::swap(lowId, highId);

        // Поиск максимального значения в диапазоне
        float maxLevel = -INFINITY;
        for (int i = lowId; i <= highId; ++i)
        {
            if (data[i] > maxLevel)
                maxLevel = data[i];
        }

        // Ограничим минимальное значение уровня
        if (maxLevel == -INFINITY || maxLevel < -150.0f)
            maxLevel = -150.0f;

        return maxLevel;
    }
    //=====================================================
    std::string genWavFileName(const std::string _templ, const double current, const int _mode)
    {
        // {yymmdd}-{uxtime_ms}-{freq}-{band}-{receivername}.wav
        // Get data
        time_t now = time(0);
        tm *ltm = localtime(&now);
        using namespace std::chrono;
        milliseconds ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch());

        std::string templ = _templ;
        double band = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

        // Format to string
        char freqStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        char bandStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        const char *type = "audio";

        sprintf(freqStr, "%.0lf", current);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year - 100);
        // sprintf(bandStr, "%02d", int(band/1000));
        sprintf(bandStr, "%d", int(band));

        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);

        // 230615-1686831173_250-107400000-300-rp10.wav
        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$u"), std::to_string(ms.count()));
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$e"), demodModeListFile[_mode]); // demodModeListEngl[_mode]);
        templ = std::regex_replace(templ, std::regex("\\$b"), bandStr);
        templ = std::regex_replace(templ, std::regex("\\$n"), thisInstance);
        templ = std::regex_replace(templ, std::regex("\\$t"), type);

        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        // templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    bool curlPOST_begin(std::string fname)
    {
        if (!use_curl)
            return false;
        if (status_direction == false)
            return false;
        std::string url = thisURL + "/begin";
        std::string payload = "fname=" + fname;

        std::thread([url, payload]()
                    {
            CURL* curl = curl_easy_init(); // создаём внутри потока!
            if (curl) {
                char curlErrorBuffer[CURL_ERROR_SIZE];
                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));
                }
                else {
                    flog::info("curl POST success to {0} with payload {1}", url, payload);
                }
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code != 200) {
                    flog::warn("Server returned HTTP code: {0}", http_code);
                }
                curl_easy_cleanup(curl);
            }
            else {
                flog::error("curl_easy_init() failed");
            } })
            .detach(); // поток уходит в фон, не блокирует

        /*
        if (curl) {
            std::string url = thisURL + "/begin";
            char curlErrorBuffer[CURL_ERROR_SIZE];
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); //"http://localhost:18101/event/begin"
            curl_easy_setopt(curl, CURLOPT_POST, 1);
            std::string payload = "fname=" + fname;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            CURLcode res = curl_easy_perform(curl);
            flog::info("curl -i -X POST  {0} {1}", url, payload.c_str());
            if (res != CURLE_OK)
                flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));
            return true;
        }
        */
        return false;
    }

    bool curlPOST_end(std::string fname)
    {
        if (!use_curl)
            return false;
        if (status_direction == false)
            return false;
        std::string url = thisURL + "/end";
        // const char *url = "http://localhost:18101/event/end";
        std::string payload = "fname=" + fname + "&uxtime=" + utils::unixTimestamp();

        std::thread([url, payload]()
                    {
            CURL* curl = curl_easy_init(); // создаём внутри потока!
            if (curl) {
                char curlErrorBuffer[CURL_ERROR_SIZE];
                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));
                }
                else {
                    flog::info("curl POST success to {0} with payload {1}", url, payload);
                }
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code != 200) {
                    flog::warn("Server returned HTTP code: {0}", http_code);
                }
                curl_easy_cleanup(curl);
            }
            else {
                flog::error("curl_easy_init() failed");
            } })
            .detach(); // поток уходит в фон, не блокирует
        /*
        if (curl) {
            // const char *url = "http://localhost:18101/event/end";
            std::string url = thisURL + "/end";
            char curlErrorBuffer[CURL_ERROR_SIZE];
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1);
            std::string payload = "fname=" + fname + "&uxtime=" + utils::unixTimestamp();
            // std::string payload = "fname=" + fname;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            flog::info("curl -i -X POST  {0} {1}", url, payload.c_str());

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK)
                flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));

            return true;
        }
        */
        return false;
    }
    //=====================================================
    std::string name;
    bool enabled = true;
    bool createOpen = false;
    bool editOpen = false;
    bool newListOpen = false;
    bool renameListOpen = false;
    bool selectListsOpen = false;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, ObservationBookmark> bookmarks;
    std::map<std::string, ObservationBookmark>::iterator itbook;
    int bookmarks_size = 0;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    ObservationBookmark editedBookmark;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;
    bool status_stop = false;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;

    int bookmarkDisplayMode = 0;

   
    std::thread workerThread;
    std::mutex scan3Mtx;

    //    double startFreq = 80000000.0;
    //    double stopFreq = 160000000.0;
    //    double interval = 10000.0;

    double current = 88000000.0;
    double curr_bw = 220000;
    double passbandRatio = 10.0;
    // int tuningTime = 300;
    //  int  _recordTime = 2;
    //  int _lingerTime = 2; // _recordTime;

    bool receiving = true;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool _record = true;
    bool _recording = false;
    bool _detected = false;
    int curr_level = -70;
    int ch_curr_Signal = -70;

    // bool flag_level = false;
    bool getflag_level[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    // int genlevel = -72;
    int getgen_level[MAX_SERVERS] = {-70, -70, -70, -70, -70, -70, -70, -70};
    std::string ch_rec_Name = "Запис";
    bool ch_recording[MAX_CHANNELS];
    std::string ch_curr_selectedVFO = "Канал приймання";

    int ch_num = 0;
    int MAX_ch_num = 0;

    std::ofstream logfile;
    std::string root = (std::string)core::args["root"];

    int devCount;
    int devId = 0;
    int devListId = 0;
    int defaultDev = 0;
    int _count = 0;

    //    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    // std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    // std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> ch_lastSignalTime[MAX_CHANNELS];
    std::chrono::time_point<std::chrono::high_resolution_clock> ch_StartTime[MAX_CHANNELS];

    std::string txtDevList;
    //     std::string currDevList;
    //    std::vector<std::string>VCardDevList;
    //    std::vector<std::string>currVCardDevList;

    std::string curr_nameWavFile[MAX_CHANNELS] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
    std::string txt_error = "";
    bool _error = false;

    std::vector<uint32_t> bandwidthsList;
    double _frec = 0;
    int _bandwidthId = 0;
    bool _raw = false;
    int NumIDBookmark = 0;

    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";
    int numInstance = 0;
    int maxCHANNELS = 6;
    int radioMode = 0;
    bool status_direction = false;
    int maxRecDuration = 5;
    int maxRecWaitTime = 10;
    int clnt_mode = -1;
    std::string source;
    std::string dop_info = "name";

    std::thread workerInfoThread;
    bool isARM = false;
    bool isServer = false;
    std::string currSource;
    uint8_t CurrSrvr;
    json ARMinstances = json({});

    bool Admin = false;
    bool correct_level = true;
    int step_srch_level = 0;
    // AutoLavel
    bool status_auto_level4 = false;
    bool initial_find_level = true;
    int cnt_skip = 0;
    double min_frec, max_frec, sigmentLeft, sigmentRight;
    const double scan_band = 12500.0; // Шаг сканирования в Гц
    bool timer_started = false;
    std::chrono::steady_clock::time_point trigger_time;
    bool use_curl = false;
    bool SignalIndf = false;
    bool status_AKF = false;
    double CentralFreq = 0;
    int maxRecShortDur_sec = 3;
    int MAX_WAIT_MS = (maxRecShortDur_sec) * 1000 + 200;
    int ADD_WAIT_MS[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::array<std::atomic<State>, MAX_SERVERS> channel_states;
    std::mutex startMtx;
    std::mutex stopMtx;
    std::atomic<bool> infoThreadStarted = false;
    std::atomic<bool> bookmarksUpdated{false};
    std::atomic<bool> listUpdater{false};
    std::mutex classMtx;
    enum class ModuleState
    {
        STOPPED,
        RUNNING,
        STARTING, // Промежуточное состояние для предотвращения двойного запуска
        STOPPING  // Промежуточное состояние для предотвращения гонок
    };
    std::atomic<ModuleState> currentState{ModuleState::STOPPED};
};

MOD_EXPORT void _INIT_()
{
    // curl_global_init(CURL_GLOBAL_ALL);
    json def = json({});
    def["selectedList"] = "General";
    def["status_AKF"] = false;
    def["status_auto_level"] = true;
    def["status_auto_level_1"] = true;
    def["status_auto_level_2"] = true;
    def["status_auto_level_3"] = true;
    def["status_auto_level_4"] = true;
    def["status_auto_level_5"] = true;
    def["status_auto_level_6"] = true;
    def["status_auto_level_7"] = true;
    def["status_auto_level_8"] = true;
    def["maxRecWaitTime"] = 5;
    def["maxRecWaitTime_1"] = 5;
    def["maxRecWaitTime_2"] = 5;
    def["maxRecWaitTime_3"] = 5;
    def["maxRecWaitTime_4"] = 5;
    def["maxRecWaitTime_5"] = 5;
    def["maxRecWaitTime_6"] = 5;
    def["maxRecWaitTime_7"] = 5;
    def["maxRecWaitTime_8"] = 5;

    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["flaglevel"] = false;
    def["lists"]["General"]["genlevel"] = -70;
    def["lists"]["General"]["Signal"] = 0;
    def["lists"]["General"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/supervision_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();
    if (!config.conf.contains("bookmarkDisplayMode"))
    {
        config.conf["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    }
    for (auto [listName, list] : config.conf["lists"].items())
    {
        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean())
        {
            continue;
        }
        json newList;
        newList = json::object();
        newList["showOnWaterfall"] = true;
        if (list.contains("flaglevel"))
        {
            newList["flaglevel"] = (bool)list["flaglevel"];
        }
        else
        {
            newList["flaglevel"] = false;
        }
        if (list.contains("genlevel"))
        {
            newList["genlevel"] = list["genlevel"];
        }
        else
        {
            newList["genlevel"] = -70;
        }

        newList["bookmarks"] = list;
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
    flog::info("MOD_EXPORT");
    /*
    gui::mainWindow.setlevel_ctrl(0, -70);
    gui::mainWindow.setlevel_ctrl(1, -70);
    gui::mainWindow.setlevel_ctrl(2, -70);
    gui::mainWindow.setlevel_ctrl(3, -70);
    gui::mainWindow.setlevel_ctrl(4, -70);
    gui::mainWindow.setlevel_ctrl(5, -70);
    gui::mainWindow.setlevel_ctrl(6, -70);
    gui::mainWindow.setlevel_ctrl(7, -70);
    */
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name)
{
    return new SupervisorModeModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance)
{
    delete (SupervisorModeModule *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
    // curl_global_cleanup();
}
