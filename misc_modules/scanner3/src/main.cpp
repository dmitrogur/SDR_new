
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <thread>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <fstream>
#include <filesystem>
#include <gui/menus/source.h>

#include <core.h>
#include <ctime>
#include <chrono>

#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;

#include <curl/curl.h>
// #define _DEBUG true
#define MIN_FREQ 25000000
#define MAX_FREQ 1700000000

#define NUM_MOD 3

#define STEP true
#define COUNT_FOR_REFIND_LEVEL 10000
#define COUNT_FOR_REFIND_SKIP 10
#define INTERVAL_FOR_FIND_Threshold 15 // min

SDRPP_MOD_INFO{
    /* Name:            */ "scanner3",
    /* Description:     */ "Observation manager module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 5, 0,
    /* Max instances    */ 1};

struct ObservationBookmark
{
    double frequency;
    float bandwidth;
    int mode;
    int level;
    bool selected;
    int Signal;
};

struct WaterfallBookmark
{
    std::string listName;
    std::string bookmarkName;
    ObservationBookmark bookmark;
};

struct ScanModeList
{
    char listName[32];
    int sizeOfList;
    int bookmarkName[MAX_COUNT_OF_DATA];
    double frequency[MAX_COUNT_OF_DATA];
    float bandwidth[MAX_COUNT_OF_DATA];
    int mode[MAX_COUNT_OF_DATA];
    int level[MAX_COUNT_OF_DATA];
    int Signal[MAX_COUNT_OF_DATA];
};

// #define MAX_LIST_PACKET_SIZE MAX_BM_SIZE * sizeof(ScanModeList)

ConfigManager config;

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

class ObservationManagerModule : public ModuleManager::Instance
{
public:
    ObservationManagerModule(std::string name)
    {
        this->name = name;
        maxRecWaitTime = 10;
        lingerTime = 5;
        bool _update = false;
        core::configManager.acquire();
        if (core::configManager.conf.contains("USE_curl"))
        {
            use_curl = core::configManager.conf["USE_curl"];
        }
        else
        {
            use_curl = true;
            // update_conf = true;
            // core::configManager.conf["USE_curl"] = use_curl;
        }
        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            radioMode = 0;
        }
        try
        {
            Admin = core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            Admin = false;
        }
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];
        thisInstance = thisInstance + "-3";
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
        try
        {
            maxRecDuration = core::configManager.conf["maxRecDuration"];
        }
        catch (const std::exception &e)
        {
            maxRecDuration = 1;
        }
        core::configManager.release(_update);

        maxRecDuration = maxRecDuration * 60;

        if (radioMode == 2)
        {
            // not registerInterface
            return;
        }
        //------------------
        _update = false;
        config.acquire();
        std::string selList = config.conf["selectedList"];
        curr_listName = selList;
        selectedListName = selList;

        try
        {
            maxRecWaitTime = config.conf["maxRecWaitTime"];
        }
        catch (...)
        {
            maxRecWaitTime = 0;
        }
        if (maxRecWaitTime == 0)
        {
            maxRecWaitTime = 5;
            config.conf["maxRecWaitTime"] = maxRecWaitTime;
            _update = true;
        }
        flog::info("maxRecWaitTime = {0}, maxRecDuration {1}", maxRecWaitTime, maxRecDuration);
        if (maxRecWaitTime >= maxRecDuration)
        {
            maxRecWaitTime = maxRecDuration - 1;
            config.conf["maxRecWaitTime"] = maxRecWaitTime;
            _update = true;
        }

        try
        {
            lingerTime = config.conf["lingerTime"];
        }
        catch (...)
        {
            lingerTime = 0;
        }
        // flog::info("lingerTime = {0}", lingerTime);

        if (lingerTime == 0)
        {
            lingerTime = 10;
            config.conf["lingerTime"] = lingerTime;
            _update = true;
        }
        if (lingerTime >= maxRecDuration)
        {
            lingerTime = maxRecDuration - 1;
            config.conf["lingerTime"] = lingerTime;
            _update = true;
        }

        // if (!isARM)
        if (config.conf.contains("glevel"))
        {
            glevel = config.conf["glevel"];
        }
        else
        {
            glevel = -70;
            _update = true;
            config.conf["glevel"] = glevel;
        }

        if (isARM)
        {
            try
            {
                status_auto_level3 = true;
                for (int i = 0; i < MAX_SERVERS; i++)
                {
                    if (i == 0)
                        status_auto_level3 = config.conf["status_auto_level_1"];
                    else if (i == 1)
                        status_auto_level3 = config.conf["status_auto_level_2"];
                    else if (i == 2)
                        status_auto_level3 = config.conf["status_auto_level_3"];
                    else if (i == 3)
                        status_auto_level3 = config.conf["status_auto_level_4"];
                    else if (i == 4)
                        status_auto_level3 = config.conf["status_auto_level_5"];
                    else if (i == 5)
                        status_auto_level3 = config.conf["status_auto_level_6"];
                    else if (i == 6)
                        status_auto_level3 = config.conf["status_auto_level_7"];
                    else if (i == 7)
                        status_auto_level3 = config.conf["status_auto_level_8"];
                    gui::mainWindow.setAuto_levelScan(i, status_auto_level3);
                }
            }
            catch (const std::exception &e)
            {
                status_auto_level3 = true;
                config.conf["status_auto_level_1"] = status_auto_level3;
                config.conf["status_auto_level_2"] = status_auto_level3;
                config.conf["status_auto_level_3"] = status_auto_level3;
                config.conf["status_auto_level_4"] = status_auto_level3;
                config.conf["status_auto_level_5"] = status_auto_level3;
                config.conf["status_auto_level_6"] = status_auto_level3;
                config.conf["status_auto_level_7"] = status_auto_level3;
                config.conf["status_auto_level_8"] = status_auto_level3;
                _update = true;
                std::cerr << e.what() << '\n';
            }
        }
        else
        {
            try
            {
                status_auto_level3 = config.conf["status_auto_level"];
            }
            catch (const std::exception &e)
            {
                status_auto_level3 = true;
                config.conf["status_auto_level"] = status_auto_level3;
                _update = true;
                std::cerr << e.what() << '\n';
            }
            gui::mainWindow.setAuto_levelScan(0, status_auto_level3);
        }

        if (_update)
            config.release(true);
        else
            config.release();
        //------------------
        flog::info("lingerTime = {0}, maxRecDuration {1}", lingerTime, maxRecDuration);
        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks(false);

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
        // bandwidthsList.push_back(250000);

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);

        root = (std::string)core::args["root"];
        flog::info("name={0}", name);
        core::modComManager.registerInterface("scanner3", name, moduleInterfaceHandler, this);
    }

    ~ObservationManagerModule()
    {
        infoThreadStarted.store(false); // Сигнал потоку на завершение
        if (workerInfoThread.joinable())
        {
            workerInfoThread.join(); // Ожидаем завершения потока
        }
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

        if (isARM)
        {
            getScanLists();
            // gui::mainWindow.setAuto_levelScan(MAX_SERVERS, status_auto_level3);
            gui::mainWindow.setLevelDbScan(MAX_SERVERS, glevel);

            gui::mainWindow.setScanListNamesTxt(listNamesTxt);
            gui::mainWindow.setUpdateScanListForBotton(gui::mainWindow.getCurrServer(), true);
        }
        else
        {
            gui::mainWindow.setAuto_levelScan(0, status_auto_level3);
            gui::mainWindow.setLevelDbScan(0, glevel);
        }
        gui::mainWindow.setUpdateModule_scan(0, false);
        flog::info("\n SCAN /currSource {0}, isARM {1}, isServer {2}", currSource, isARM, isServer);
        gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, maxRecWaitTime);
        gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, lingerTime);
        /*
        if (isServer || isARM)
        {
            workerInfoRunning = true;
            workerInfoThread = std::thread(&ObservationManagerModule::workerInfo, this);
        }
        */
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
    static void workerInfo(void *ctx)
    {
        ObservationManagerModule *_this = (ObservationManagerModule *)ctx;
        // if (currSource != SOURCE_ARM) return;
        std::mutex scanInfoMtx;
        while (_this->infoThreadStarted.load())
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            {
                std::lock_guard<std::mutex> lck(scanInfoMtx);
                uint8_t currSrvr = gui::mainWindow.getCurrServer();
                _this->CurrSrvr = currSrvr;

                if (_this->restart_requested.load())
                {
                    flog::info("Watchdog: Restart request detected. Executing stop() -> start().");

                    // Сбрасываем флаг, чтобы не делать это снова
                    _this->restart_requested.store(false);
                    _this->stop();
                    _this->_recording.store(false);
                    _this->_Receiving = false;
                    _this->start();
                    continue;
                }

                // flog::info("currSrvr {0}, _this->selectedListId {1}, getidOfList_scan {2}", currSrvr, _this->selectedListId, gui::mainWindow.getidOfList_scan(currSrvr));
                if (_this->isARM)
                {
                    if (_this->selectedListId != gui::mainWindow.getidOfList_scan(currSrvr) && !gui::mainWindow.getUpdateMenuSnd6Scan(currSrvr))
                    {
                        if (gui::mainWindow.getidOfList_scan(currSrvr) < _this->listNames.size())
                            _this->selectedListId = gui::mainWindow.getidOfList_scan(currSrvr);
                        else
                            _this->selectedListId = 0;
                        _this->loadByName(_this->listNames[_this->selectedListId]);
                        config.acquire();
                        config.conf["selectedList"] = _this->selectedListName;
                        config.release(true);
                        _this->refreshWaterfallBookmarks(false);
                    }
                    if (_this->status_auto_level3)
                    {
                        if (_this->curr_level != gui::mainWindow.getLevelDbScan(currSrvr))
                        {
                            _this->curr_level = gui::mainWindow.getLevelDbScan(currSrvr);
                        }
                    }
                    else
                    {
                        if (_this->glevel != gui::mainWindow.getLevelDbScan(currSrvr))
                        {
                            _this->glevel = gui::mainWindow.getLevelDbScan(currSrvr);
                            _this->glevel = std::clamp<int>(_this->glevel, -150, -30);
                            _this->curr_level = _this->glevel;
                        }
                    }
                    if (_this->status_auto_level3 != gui::mainWindow.getAuto_levelScan(currSrvr))
                    {
                        _this->status_auto_level3 = gui::mainWindow.getAuto_levelScan(currSrvr);
                        config.acquire();
                        config.conf["status_auto_level"] = _this->status_auto_level3;
                        config.release(true);
                        flog::info("UPDATE _this->status_auto_level {0}", _this->status_auto_level3);
                    }
                }
                if (_this->isServer)
                {
                    if (gui::mainWindow.getUpdateMenuRcv6Scan())
                    {
                        _this->currSource = sourcemenu::getCurrSource();
                        flog::info("gui::mainWindow.getUpdateMenuRcv6Scan()");
                        gui::mainWindow.setUpdateMenuRcv6Scan(false);
                        bool _update = false;
                        if (gui::mainWindow.getUpdateListRcv6Scan(currSrvr))
                        {
                            flog::info("gui::mainWindow.getUpdateListRcv6Scan()");
                            gui::mainWindow.setUpdateListRcv6Scan(currSrvr, false);
                            config.acquire();
                            for (auto it = _this->listNames.begin(); it != _this->listNames.end(); ++it)
                            {
                                // listNames->first;
                                std::string name = *it;
                                if (name != "General")
                                {
                                    // flog::info(" delete listName = {0}...", name);
                                    config.conf["lists"].erase(name);
                                }
                            }
                            config.release(true);

                            int cnt_bbuf = gui::mainWindow.getsizeOfbbuf_scan();
                            flog::info("gui::mainWindow.getUpdateListRcv6Scan() cnt_bbuf = {0}", cnt_bbuf);

                            void *bbufRCV = ::operator new(cnt_bbuf);
                            memcpy(bbufRCV, gui::mainWindow.getbbuf_scan(), cnt_bbuf);
                            config.acquire();
                            ScanModeList sbm;
                            for (int poz = 0; poz < cnt_bbuf; poz = poz + sizeof(sbm))
                            {
                                memcpy(&sbm, ((uint8_t *)bbufRCV) + poz, sizeof(sbm));
                                std::string listname = std::string(sbm.listName);
                                // flog::info("!!!! poz {0}, sbm.listName {1}, listname {1}, sizeOfList {2}  ", poz, sbm.listName, sbm.sizeOfList);
                                json def;
                                def = json::object();
                                for (int i = 0; i < sbm.sizeOfList; i++)
                                {
                                    std::string bookmarkname = std::to_string(sbm.bookmarkName[i]);
                                    def["bookmarks"][bookmarkname]["frequency"] = sbm.frequency[i];
                                    def["bookmarks"][bookmarkname]["bandwidth"] = sbm.bandwidth[i];
                                    def["bookmarks"][bookmarkname]["level"] = sbm.level[i];
                                    def["bookmarks"][bookmarkname]["mode"] = sbm.mode[i];
                                    def["bookmarks"][bookmarkname]["Signal"] = sbm.Signal[i];
                                }
                                def["showOnWaterfall"] = true;
                                // config.conf["lists"][listname] = true;
                                config.conf["lists"][listname] = def;
                                /// flog::info("!!!! poz {0}, sbm.listName {1}, listname {1}  ", poz, sbm.listName, listname);
                            }
                            config.release(true);
                            ::operator delete(bbufRCV);
                            _this->refreshLists();

                            gui::mainWindow.setScanListNamesTxt(_this->listNamesTxt);
                            gui::mainWindow.setUpdateScanListForBotton(currSrvr, true);
                            // gui::mainWindow.setScanListNamesTxt(currSrvr, _this->listNamesTxt);
                            _update = true;
                        }
                        if (_update || (_this->selectedListId != gui::mainWindow.getidOfList_scan(currSrvr) && !gui::mainWindow.getUpdateMenuSnd6Scan(currSrvr)))
                        {
                            flog::info("currSrvr {0}, _this->selectedListId {1}, getidOfList_scan {2}, listNames.size() {3}", currSrvr, _this->selectedListId, gui::mainWindow.getidOfList_scan(currSrvr), _this->listNames.size());
                            if (gui::mainWindow.getidOfList_scan(currSrvr) < _this->listNames.size())
                                _this->selectedListId = gui::mainWindow.getidOfList_scan(currSrvr);
                            else
                                _this->selectedListId = 0;
                            // flog::info("currSrvr {0}, _this->selectedListId {1}, getidOfList_scan {2}", currSrvr, _this->selectedListId, gui::mainWindow.getidOfList_scan(currSrvr));
                            _this->loadByName(_this->listNames[_this->selectedListId]);
                            config.acquire();
                            config.conf["selectedList"] = _this->selectedListName;
                            config.release(true);
                        }
                        if (gui::mainWindow.getUpdateModule_scan(currSrvr))
                        {
                            gui::mainWindow.setUpdateModule_scan(currSrvr, false);
                        }
                        bool arm_run = gui::mainWindow.getbutton_scan(currSrvr);
                        if (_this->running.load() != arm_run)
                        {
                            flog::info("SCANNER3 arm_run {0}, _this->running.load {1} ", arm_run, _this->running.load());
                            int _air_recording;
                            core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
                            if (!_this->running.load())
                            {
                                if (_air_recording == 1)
                                {
                                    _this->start();
                                }
                            }
                            else
                            {
                                _this->stop();
                                _this->_recording.store(false);
                                _this->_Receiving = false;
                                _this->_detected.store(false);
                            }
                        }
                    }

                    if (gui::mainWindow.getMaxRecWaitTime_scan(currSrvr) != _this->maxRecWaitTime)
                    {
                        _this->maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_scan(currSrvr);
                        config.acquire();
                        config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
                        config.release(true);
                    }
                    if (gui::mainWindow.getMaxRecDuration_scan(currSrvr) != _this->lingerTime)
                    {
                        _this->lingerTime = gui::mainWindow.getMaxRecDuration_scan(currSrvr);
                        config.acquire();
                        config.conf["lingerTime"] = _this->lingerTime;
                        config.release(true);
                    }
                    if (_this->status_auto_level3 != gui::mainWindow.getAuto_levelScan(currSrvr))
                    {
                        _this->status_auto_level3 = gui::mainWindow.getAuto_levelScan(currSrvr);
                        config.acquire();
                        config.conf["status_auto_level"] = _this->status_auto_level3;
                        config.release(true);
                    }
                    if (!_this->status_auto_level3)
                    {
                        if (_this->glevel != gui::mainWindow.getLevelDbScan(currSrvr))
                        {
                            _this->glevel = gui::mainWindow.getLevelDbScan(currSrvr);
                            _this->glevel = std::clamp<int>(_this->glevel, -150, -30);
                            _this->curr_level = _this->glevel;
                            config.acquire();
                            config.conf["glevel"] = _this->glevel;
                            config.release(true);
                        }
                    }
                }
            }
        }
    }

    static void applyMode(ObservationBookmark bm, std::string vfoName)
    {
        if (vfoName != "")
        {
            if (core::modComManager.interfaceExists(vfoName))
            {
                /*
                double curr_freq = gui::waterfall.getCenterFrequency(); //  + gui::waterfall.vfos["Канал приймання"]->generalOffset;
                flog::warn("curr_freq {0}, bm.frequency {1}", curr_freq, bm.frequency);
                double  new_freq= bm.frequency;
                if (new_freq!= curr_freq)
                {
                    flog::warn("Tunning... curr_freq {0} != bm.frequency {1}", curr_freq, new_freq);
                    // gui::waterfall.centerFreqMoved = false;
                    //if (!gui::waterfall.selectedVFO.empty())
                    //{
                        // std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        tuner::centerTuning(gui::waterfall.selectedVFO, new_freq);
                        flog::warn("Tunning...  OK! curr_freq {0} != bm.frequency {1}", curr_freq, new_freq);
                        tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, new_freq);
                    //}
                    flog::warn("Tunning...  OK! curr_freq {0} != bm.frequency {1}", curr_freq, new_freq);
                    gui::waterfall.centerFreqMoved = true;

                    gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
                    // std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                */
                if (core::modComManager.getModuleName(vfoName) == "radio")
                {
                    int mode = bm.mode;
                    double bandwidth = (double)bm.bandwidth;

                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            //            tuner::normalTuning(gui::waterfall.selectedVFO, bm.frequency);
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
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }
    }

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

    // Генерация уникального имени
    std::string makeUniqueName(const std::string &rawName, const std::vector<std::string> &listNames)
    {
        // Встроенная функция toLower
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
        // Очищенное базовое имя
        std::string baseName = removeSpecialChars(rawName.c_str());
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
        return uniqueName;
    }

    bool bookmarkEditDialog()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_3" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[32];
        strcpy(nameBuf, editedBookmarkName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Назва");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputText(("##freq_manager_edit_name_3" + name).c_str(), nameBuf, 32))
            {
                editedBookmarkName = removeSpecialChars(nameBuf);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Частота, кГц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            //            ImGui::InputDouble(("##freq_manager_edit_freq_3" + name).c_str(), &editedBookmark.frequency);
            /*
            if(ImGui::InputInt(("##freq_manager_edit_freq_3" + name).c_str(), &_frec, 10, 100)) {
                _frec= std::clamp<int>(_frec, 1, 999999999);
            };
            */
            if (ImGui::InputDouble("##req_manager_edit_freq_3", &_frec, 1.0f, 100.0f, "%.3f"))
            {
                _frec = std::clamp<double>(_frec, 10000.0, 1999999.0);
            }
            ImGui::SameLine();
            if (ImGui::Button(".000"))
            { // 0xF7 247
                _frec = (double)round(_frec);
            }

            /*
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Смуга, кГц      ");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            // ImGui::InputDouble(("##freq_manager_edit_bw_3" + name).c_str(), &editedBookmark.bandwidth);

            if (ImGui::Combo(("##freq_manager_edit_bw_3" + name).c_str(), &_bandwidthId, bandListTxt))
            {
                editedBookmark.bandwidth = bandwidthsList[_bandwidthId];

                flog::info("TRACE. editedBookmark.bandwidth = {0} !", editedBookmark.bandwidth);
            }
            // flog::info("Add 2 bandwidth {0}", editedBookmark.bandwidth);
            */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Смуга, кГц      ");

            float kHzbandwidth = 1.0;
            if (editedBookmark.bandwidth < 1000)
                kHzbandwidth = 1.0;
            else
                kHzbandwidth = editedBookmark.bandwidth / 1000.0;
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);

            if (ImGui::InputFloat(("##freq_manager_edit_bw4_" + name).c_str(), &kHzbandwidth, 1.25, 10.0, "%.2f"))
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

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Вид демод.");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            // ImGui::Combo(("##freq_manager_edit_mode_3" + name).c_str(), &editedBookmark.mode, demodModeListTxt);
            if (ImGui::Combo(("##freq_manager_edit_mode_3" + name).c_str(), &editedBookmark.mode, demodModeListTxt))
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
                flog::info("TRACE. editedBookmark.mode = {0}, getSampleRate() {1} !", editedBookmark.mode, sigpath::iqFrontEnd.getSampleRate());
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Тип сигналу");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo(("##supervisor_edit_signal_3" + name).c_str(), &editedBookmark.Signal, SignalListTxt))
            {
                editedBookmark.Signal = editedBookmark.Signal;
                if (editedBookmark.Signal < 0 || editedBookmark.Signal > 2)
                {
                    editedBookmark.Signal = 0;
                }

                flog::info("TRACE. editedBookmark.mode = {0} !", editedBookmark.mode);
            }
            editedBookmark.level = -70;
            // std::clamp<int>(editedBookmark.level, -150, -30);

            ImGui::EndTable();

            int type_error = 0;
            bool applyDisabled = (strlen(nameBuf) == 0) || _raw == true || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);

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
                if (editOpen == false)
                {
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (bm.frequency == editedBookmark.frequency && bm.mode == editedBookmark.mode)
                        {
                            applyDisabled = true;
                            type_error = 1;
                            break;
                        }
                    }
                }
                else
                { // if (editOpen == true) {
                    int cnt = 0;
                    for (auto &[name, bm] : bookmarks)
                    {
                        if (bm.frequency == editedBookmark.frequency && bm.mode == editedBookmark.mode && NumIDBookmark != cnt)
                        {
                            // flog::info("bm.frequency {0} == editedBookmark.frequency {1}, cnt {2}", bm.frequency, editedBookmark.frequency, cnt);
                            applyDisabled = true;
                            type_error = 1;
                            break;
                        }
                        cnt++;
                    }
                }
            }

            if (applyDisabled)
            {
                style::beginDisabled();
            }
            if (ImGui::Button("   OK   "))
            {
                open = false;
                editedBookmark.frequency = round(_frec * 1000);
                flog::info("Add 3 bandwidth {0}", editedBookmark.bandwidth);

                // If editing, delete the original one
                if (editOpen)
                {
                    bookmarks.erase(firstEditedBookmarkName);
                }
                bookmarks[editedBookmarkName] = editedBookmark;

                saveByName(selectedListName);

                ObservationBookmark &bm = bookmarks[editedBookmarkName];
                applyBookmark(bm, gui::waterfall.selectedVFO);
                bm.selected = false;
                getScanLists();
                gui::mainWindow.setScanListNamesTxt(listNamesTxt);
                gui::mainWindow.setUpdateModule_scan(CurrSrvr, true);
                flog::info("!!!! getScanLists(), CurrSrvr {0}", CurrSrvr);
                gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
                gui::mainWindow.setUpdateListRcv6Scan(CurrSrvr, true);
                gui::mainWindow.setUpdateScanListForBotton(CurrSrvr, true);
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

    bool newListDialog()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##freq_manager_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[32];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::LeftLabel("Назва");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name_3" + name).c_str(), nameBuf, 32))
            {
                editedListName = makeUniqueName(std::string(nameBuf), listNames);
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists)
            {
                style::beginDisabled();
            }
            if (ImGui::Button("   OK   "))
            {
                open = false;
                waterfallBookmarks.clear();

                config.acquire();
                if (renameListOpen)
                {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else
                {
                    config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                config.release(true);
                refreshWaterfallBookmarks(false);
                refreshLists();
                loadByName(editedListName);
                getScanLists();
                gui::mainWindow.setScanListNamesTxt(listNamesTxt);
                gui::mainWindow.setUpdateModule_scan(CurrSrvr, true);
                flog::info("!!!! getScanLists()");
                gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
                gui::mainWindow.setUpdateListRcv6Scan(CurrSrvr, true);
                gui::mainWindow.setUpdateScanListForBotton(CurrSrvr, true);
                curr_listName = editedListName;
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

        std::string id = "Вибір Банку##freq_manager_sel_popup_3" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items())
            {
                bool shown = list["showOnWaterfall"];
                config.conf["lists"][listName]["showOnWaterfall"] = false;
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_3").c_str(), &shown))
                {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    config.release(true);
                    refreshWaterfallBookmarks(false);
                    curr_listName = listName;
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
            std::string _name = key; // makeUniqueName(key, listNames);
            listNames.push_back(_name);
            // flog::info("listNames _name'{0}'", _name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void refreshWaterfallBookmarks(bool lockConfig = true)
    {
        // if (lockConfig) { config.acquire(); }
        config.acquire();
        waterfallBookmarks.clear();
        for (auto [listName, list] : config.conf["lists"].items())
        {
            if (!((bool)list["showOnWaterfall"]))
            {
                continue;
            }

            if (listName != curr_listName)
                continue;
            // flog::info("listName = {0}", listName.c_str());
            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items())
            {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark.frequency = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                wbm.bookmark.bandwidth = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                wbm.bookmark.mode = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                try
                {
                    wbm.bookmark.Signal = config.conf["lists"][listName]["bookmarks"][bookmarkName]["Signal"];
                }
                catch (const std::exception &e)
                {
                    std::cerr << e.what() << '\n';
                    wbm.bookmark.Signal = 0;
                }

                wbm.bookmark.level = -70; // std::clamp<int>(wbm.bookmark.level, -150, -30);

                wbm.bookmark.selected = false;
                waterfallBookmarks.push_back(wbm);
            }
        }
        // if (lockConfig) { config.release(); }
        config.release();
    }

    void getScanLists()
    {
        // flog::info("    getScanLists");
        void *bbuf = ::operator new(MAX_LIST_PACKET_SCAN_SIZE); // new uint8_t[MAX_PACKET_SIZE];
        int sizeofbbuf = 0;
        ScanModeList sbm;
        config.acquire();
        {
            int count = 0;
            for (auto [listName, list] : config.conf["lists"].items())
            {
                // flog::info("listName ={0}", listName.c_str());
                // WaterfallBookmark wbm;
                strcpy(sbm.listName, listName.c_str());
                // sbm.listName = listName;
                int i = 0;
                for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items())
                {
                    try
                    {
                        sbm.bookmarkName[i] = std::stoi(bookmarkName);
                    }
                    catch (...)
                    {
                        sbm.bookmarkName[i] = i;
                    }
                    // flog::info("{0}, bookmarkName ={1}", i, sbm.bookmarkName[i]);

                    sbm.frequency[i] = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                    // flog::info("sbm.frequency[{0}] = {1}", i, sbm.frequency[i]);
                    sbm.bandwidth[i] = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                    sbm.mode[i] = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                    try
                    {
                        sbm.Signal[i] = config.conf["lists"][listName]["bookmarks"][bookmarkName]["Signal"];
                    }
                    catch (const std::exception &e)
                    {
                        sbm.Signal[i] = 0;
                        std::cerr << e.what() << '\n';
                    }
                    sbm.level[i] = -70; // std::clamp<int>(sbm.level[i], -150, -30);

                    i++;
                    if (i >= MAX_COUNT_OF_DATA)
                        break;
                }
                sbm.sizeOfList = i;
                flog::info("{0}. sbm.listName {1}, sbm.sizeOfList {2}", count, sbm.listName, sbm.sizeOfList);
                memcpy(((uint8_t *)bbuf) + sizeofbbuf, (void *)&sbm, sizeof(sbm));
                sizeofbbuf = sizeofbbuf + sizeof(sbm);
                count++;
                if (count >= MAX_BM_SIZE)
                    break;
            }
        }
        config.release();
        // gui::mainWindow.setselectedLogicId(CurrSrvr, selectedLogicId);
        gui::mainWindow.setbbuf_scan(bbuf, sizeofbbuf);
        ::operator delete(bbuf);
        gui::mainWindow.setidOfList_scan(CurrSrvr, selectedListId);
        gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, maxRecWaitTime);
        gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, lingerTime);
        gui::mainWindow.setLevelDbScan(CurrSrvr, glevel);
        gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
        flog::warn("SCAN (msgScan) sizeofbbuf: {0}, gui::mainWindow.getUpdateMenuSnd6Scan {1}", sizeofbbuf, gui::mainWindow.getUpdateMenuSnd6Scan(CurrSrvr));
    }

    void loadFirst()
    {
        if (listNames.size() > 0)
        {
            loadByName(listNames[0]);
            refreshWaterfallBookmarks(false);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName)
    {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end())
        {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }

        selectedListName = listName;
        curr_listName = listName;
        config.acquire();
        bool update = false;
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items())
        {
            try
            {
                if (bm["frequency"] == "")
                    break;
            }
            catch (...)
            {
                flog::warn("loadByName listName {0} is emply", listName);
                break;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            try
            {
                fbm.level = bm["level"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.level = -50;
            }
            fbm.level = std::clamp<int>(fbm.level, -150, -30);
            try
            {
                fbm.Signal = bm["Signal"];
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                fbm.Signal = 0;
                bm["Signal"] = fbm.Signal;
                update = true;
            }
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release(update);
        bookmarks_size = bookmarks.size();
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        flog::info("!!!! loadByName() {0} listName={1}", selectedListId, listName);
    }

    void saveByName(std::string listName)
    {
        int i = 0;
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks)
        {
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = bm.mode;
            bm.level = std::clamp<int>(bm.level, -150, -30);
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = bm.mode;
            config.conf["lists"][listName]["bookmarks"][bmName]["Signal"] = bm.Signal;
            i++;
            // flog::info("{0}, bm.frequency {1}", i, bm.frequency);
        }
        config.release(true);
        refreshWaterfallBookmarks(false);
    }

    bool menuDialog()
    {
        bool open = true;
        // flog::warn("bookmarks is emply 2!");
        gui::mainWindow.lockWaterfallControls = true;
        std::string id = "Delete##del_item_wf";
        ImGui::OpenPopup(id.c_str());
        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::Text("Видалити відмічену частоту? Ви впевнені?");
            if (ImGui::Button("   OK   "))
            {
                open = false;
            }
            if (ImGui::Button("Скасувати"))
            {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    static void menuHandler(void *ctx)
    {
        ObservationManagerModule *_this = (ObservationManagerModule *)ctx;
        _this->currSource = sourcemenu::getCurrSource();
        uint8_t currSrvr = gui::mainWindow.getCurrServer();
        _this->CurrSrvr = currSrvr;

        float menuWidth = ImGui::GetContentRegionAvail().x;
        int _air_recording;
        int _work;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
        int _run = _this->running.load();
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float btnSize = 0; // ImGui::CalcTextSize("Зберегти як...").x + 8;
        int _count_freq = 0;

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto &[name, bm] : _this->bookmarks)
        {
            if (bm.selected)
            {
                selectedNames.push_back(name);
            }
        }

        bool showThisMenu = _this->isServer;
        if (_this->Admin)
            showThisMenu = false;
        if (_run || _work > 0 || showThisMenu)
        {
            ImGui::BeginDisabled();
        }
        std::string _ListName = _this->listNames[_this->selectedListId];
        bool FirstBank = (_this->listNames[_this->selectedListId] == "General") ? true : false;
        bool NullBank = false;
        if (_this->listNames.size() == 0 || _ListName == "")
            NullBank = true;
        bool ActiveUseBank = false;
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            if (gui::mainWindow.getbutton_scan(i) == true && gui::mainWindow.getidOfList_scan(i) == _this->selectedListId)
            {
                ActiveUseBank = true;
                break;
            }
        }

        btnSize = ImGui::CalcTextSize("Зберегти як...").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel_3" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str()))
        {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
            config.conf["lingerTime"] = _this->lingerTime;
            config.release(true);
            _this->refreshWaterfallBookmarks(false);
            gui::mainWindow.setidOfList_scan(currSrvr, _this->selectedListId);
            gui::mainWindow.setUpdateMenuSnd6Scan(currSrvr, true);
        }
        if (NullBank || FirstBank || ActiveUseBank)
        {
            style::beginDisabled();
        }
        {
            ImGui::SameLine();
            if (ImGui::Button(("Зберегти як...##_freq_mgr_ren_lst_3" + _this->name).c_str(), ImVec2(btnSize, 0)))
            {
                _this->firstEditedListName = _this->listNames[_this->selectedListId];
                _this->editedListName = _this->firstEditedListName;
                _this->renameListOpen = true;
            }
        }
        if (NullBank || FirstBank || ActiveUseBank)
        {
            style::endDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0)))
        {
            // Find new unique default name
            _this->editedListName = "New List";
            _this->makeUniqueName(_this->editedListName, _this->listNames);
            _this->newListOpen = true;
        }

        if (NullBank || FirstBank || ActiveUseBank)
        {
            style::beginDisabled();
        }
        {
            ImGui::SameLine();
            // if (_this->selectedListName == "" || FirstBank) { style::beginDisabled(); }
            if (ImGui::Button(("-##_freq_mgr_del_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0)))
            {
                _this->deleteListOpen = true;
            }
        }
        if (NullBank || FirstBank || ActiveUseBank)
        {
            style::endDisabled();
        }

        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm3" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]()
                                 { ImGui::Text(" \"%s\". Ви впевнені?", _this->selectedListName.c_str()); }) == GENERIC_DIALOG_BUTTON_YES)
        {
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            config.release(true);
            _this->refreshWaterfallBookmarks(false);
            _this->refreshLists();
            _this->getScanLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0)
            {
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else
            {
                _this->selectedListName = "";
            }
            gui::mainWindow.setidOfList_scan(currSrvr, _this->selectedListId);
            gui::mainWindow.setScanListNamesTxt(_this->listNamesTxt);
            gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
            gui::mainWindow.setUpdateListRcv6Scan(currSrvr, true);
            gui::mainWindow.setUpdateScanListForBotton(currSrvr, true);
        }
        //=====================================================================================
        // Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table3" + _this->name).c_str(), 3);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (FirstBank)
        {
            style::beginDisabled();
        }
        {

            // Імпорт 1
            if (ImGui::Button(("Завантажити##_freq_mgr_imp_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen)
            {
                std::string pathValid = ""; //  core::configManager.getPath() +"/Banks/*";
                core::configManager.acquire();
                std::string jsonPath = core::configManager.conf["PathJson"];
                core::configManager.release();

                try
                {
                    fs::create_directory(jsonPath);
                    pathValid = jsonPath + "/Scan";
                    fs::create_directory(pathValid);
                    pathValid = pathValid + "/*";
                    _this->importOpen = true;
                    _this->AddImport = false;
                }
                catch (const std::exception &e)
                {
                    std::cerr << e.what() << '\n';
                }
                if (_this->importOpen)
                    _this->importDialog = new pfd::open_file("Import bookmarks", pathValid, {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
            }
        }
        if (FirstBank)
        {
            style::endDisabled();
        }

        ImGui::TableSetColumnIndex(1);
        // Імпорт 2 (Add)
        if (ImGui::Button(("Додати в банк##_freq_mgr_imp_add_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen)
        {
            std::string pathValid = ""; //  core::configManager.getPath() +"/Banks/*";
            core::configManager.acquire();
            std::string jsonPath = core::configManager.conf["PathJson"];
            core::configManager.release();

            try
            {
                fs::create_directory(jsonPath);
                pathValid = jsonPath + "/Scan";
                fs::create_directory(pathValid);
                pathValid = pathValid + "/*";
                _this->importOpen = true;
                _this->AddImport = true;
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
            }
            if (_this->importOpen)
                _this->importDialog = new pfd::open_file("Import bookmarks", pathValid, {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
        }

        ImGui::TableSetColumnIndex(2);
        if (NullBank || FirstBank)
        {
            style::beginDisabled();
        }
        {
            // Експорт
            if (ImGui::Button(("Зберегти у файл##_freq_mgr_exp_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen)
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
                    pathValid = jsonPath + "/Scan";
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
                    std::string expname = pathValid + "/scan_" + std::to_string(InstNum) + "_" + _this->selectedListName + ".json";
                    _this->exportedBookmarks["domain"] = "freqs-bank";
                    _this->exportedBookmarks["rx-mode"] = "scanning";
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
                    for (auto [bmName, bm] : config.conf["lists"][_this->selectedListName]["bookmarks"].items())
                    {
                        _this->exportedBookmarks[_this->selectedListName]["bookmarks"][bmName] = config.conf["lists"][_this->selectedListName]["bookmarks"][bmName];
                    }
                    config.release();

                    flog::info("3. expname {0}", expname);
                    _this->exportOpen = true;
                    _this->exportDialog = new pfd::save_file("Export bookmarks", expname.c_str(), {"JSON Files (*.json)", "*.json", "All Files", "*"}, pfd::opt::multiselect);
                }
                catch (const std::exception &e)
                {
                    std::cerr << e.what() << '\n';
                }
            }
        }
        if (NullBank || FirstBank)
        {
            style::endDisabled();
        }

        ImGui::EndTable();

        // if (showThisMenu) { ImGui::BeginDisabled(); } //---------------------------------
        // Bookmark list
        if (ImGui::BeginTable(("freq_manager_bkm_table_3" + _this->name).c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 150)))
        { // | ImGuiTableFlags_ScrollY
            ImGui::TableSetupColumn("Назва");
            ImGui::TableSetupColumn("Частота, МГц");
            ImGui::TableSetupColumn("Вид демод.");
            ImGui::TableSetupColumn("Смуга, кГц");
            // ImGui::TableSetupColumn("Поріг, дБ");
            ImGui::TableSetupColumn("Тип сигналу");

            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            for (auto &[name, bm] : _this->bookmarks)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // ImVec2 min = ImGui::GetCursorPos();

                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_3" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick))
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

                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered())
                {
                    // applyBookmark(bm, gui::waterfall.selectedVFO);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        applyBookmark(bm, gui::waterfall.selectedVFO);
                    }
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    {
                        // _this->menuListOpen = true;
                        // menuListOpen = _this->menuDialog();
                    }
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", utils::formatFreqMHz(bm.frequency).c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", demodModeList[bm.mode]);

                ImGui::TableSetColumnIndex(3);
                int bw = round(bm.bandwidth / 1000);
                std::string sbw = std::to_string(bw);
                if (bw == 13)
                    sbw = "12.5";
                if (bw == 6)
                    sbw = "6.25";

                ImGui::Text("%s", sbw.c_str());

                ImGui::TableSetColumnIndex(4);
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
                // ImGui::Text("%s", std::to_string(bm.level).c_str());

                ImVec2 max = ImGui::GetCursorPos();
                _count_freq++;
            }
            ImGui::EndTable();
        }

        // if (!showThisMenu) { // _this->isServer
        // Draw buttons on top of the list
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 3);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Додати##_freq_mgr_add_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            // If there's no VFO selected, just save the center freq
            if (gui::waterfall.selectedVFO == "")
            {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency();
                _this->editedBookmark.bandwidth = 0;
                _this->_bandwidthId = 0;
                _this->editedBookmark.mode = 7;
            }
            else
            { // showOnWaterfall
                if (gui::mainWindow.gettuningMode() == tuner::TUNER_MODE_NORMAL)
                {
                    gui::mainWindow.settuningMode(tuner::TUNER_MODE_CENTER);
                    gui::waterfall.VFOMoveSingleClick = true;
                    tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);

                    // gui::waterfall.centerFreqMoved = true;
                    usleep(100);
                    flog::info("TUNER_MODE_CENTER");
                    gui::mainWindow.settuningMode(tuner::TUNER_MODE_NORMAL);
                    gui::waterfall.VFOMoveSingleClick = false;
                    gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
                }
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);

                float bw = _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                for (int i = 1; i < _this->bandwidthsList.size(); i++)
                {
                    _this->editedBookmark.bandwidth;
                    if (bw > _this->bandwidthsList[i - 1] && bw <= _this->bandwidthsList[i])
                    {
                        flog::info("TRACE!!! scanner3 bw old  = {0}, bw new = {1}, andwidthId = {1}!", bw, _this->bandwidthsList[i], i);
                        if (bw < _this->bandwidthsList[i - 1] + 10)
                        {
                            bw = _this->bandwidthsList[i - 1];
                            _this->_bandwidthId = i - 1;
                        }
                        else
                        {
                            bw = _this->bandwidthsList[i];
                            _this->_bandwidthId = i;
                        }
                        break;
                    }
                }
                _this->editedBookmark.bandwidth = bw;

                _this->editedBookmark.mode = 7;
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio")
                {
                    int mode = gui::mainWindow.getselectedDemodID();
                    // core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
            }
            _this->_frec = _this->editedBookmark.frequency / 1000.0;
            // _this->_bandwidthId = 0;
            _this->_raw = false;
            flog::info("Add bandwidth {0}", _this->editedBookmark.bandwidth);
            _this->glevel = std::clamp<int>(_this->glevel, -150, -30);
            int maxLevel = (int)_this->glevel;
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
            // if(maxLevel<-150.0) maxLevel=-150;

            _this->editedBookmark.level = maxLevel;
            _this->editedBookmark.Signal = 0;

            _this->editedBookmark.selected = false;

            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("1") == _this->bookmarks.end())
            {
                _this->editedBookmarkName = "1";
            }
            else
            {
                char buf[64];
                for (int i = 2; i < 1000; i++)
                {
                    sprintf(buf, "%d", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end())
                    {
                        break;
                    }
                }
                _this->editedBookmarkName = _this->removeSpecialChars(buf);
            }
        }

        if (NullBank)
        {
            style::beginDisabled();
        }
        //--------------------------------------------------->
        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() != 1 && _this->selectedListName != "")
            style::beginDisabled();
        if (ImGui::Button(("Видалити##_freq_mgr_rem_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "")
            style::endDisabled();

        ImGui::TableSetColumnIndex(2);

        if (selectedNames.size() != 1 && _this->selectedListName != "")
            style::beginDisabled();
        if (ImGui::Button(("Редаг##_freq_mgr_edt_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            _this->editOpen = true;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];
            _this->_frec = _this->editedBookmark.frequency / 1000.0;
            _this->_bandwidthId = 0;
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
        }

        if (selectedNames.size() != 1 && _this->selectedListName != "")
            style::endDisabled();

        ImGui::EndTable();
        //==============================================================
        // Bookmark delete confirm dialog
        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]()
                                 { ImGui::TextUnformatted("Видалити відмічену частоту? Ви впевнені?"); }) == GENERIC_DIALOG_BUTTON_YES)
        {
            for (auto &_name : selectedNames)
            {
                _this->bookmarks.erase(_name);
            }
            _this->saveByName(_this->selectedListName);
            _this->getScanLists();
            gui::mainWindow.setScanListNamesTxt(_this->listNamesTxt);
            gui::mainWindow.setUpdateModule_scan(_this->CurrSrvr, true);
            flog::info("!!!! getScanLists(), CurrSrvr {0}", _this->CurrSrvr);
            gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
            gui::mainWindow.setUpdateListRcv6Scan(_this->CurrSrvr, true);
            gui::mainWindow.setUpdateScanListForBotton(_this->CurrSrvr, true);
        }

        //-----------------------------------------------------------------
        // _this->flag_level = true;

        if (ImGui::Checkbox("Автоматичне визначення порогу ##_auto_level_3", &_this->status_auto_level3))
        {
            config.acquire();
            if (_this->isARM)
            {
                if (currSrvr == 0)
                    config.conf["status_auto_level_1"] = _this->status_auto_level3;
                else if (currSrvr == 1)
                    config.conf["status_auto_level_2"] = _this->status_auto_level3;
                else if (currSrvr == 2)
                    config.conf["status_auto_level_3"] = _this->status_auto_level3;
                else if (currSrvr == 3)
                    config.conf["status_auto_level_4"] = _this->status_auto_level3;
                else if (currSrvr == 4)
                    config.conf["status_auto_level_5"] = _this->status_auto_level3;
                else if (currSrvr == 5)
                    config.conf["status_auto_level_6"] = _this->status_auto_level3;
                else if (currSrvr == 6)
                    config.conf["status_auto_level_7"] = _this->status_auto_level3;
                else if (currSrvr == 7)
                    config.conf["status_auto_level_8"] = _this->status_auto_level3;
            }
            else
            {
                config.conf["status_auto_level"] = _this->status_auto_level3;
            }
            config.release(true);

            gui::mainWindow.setAuto_levelScan(currSrvr, _this->status_auto_level3);
            if (_this->status_auto_level3)
            {
                //_this->curr_level = _this->level;
                gui::mainWindow.setLevelDbScan(currSrvr, _this->curr_level);
            }
            gui::mainWindow.setUpdateMenuSnd6Scan(currSrvr, true);
        }

        ImGui::LeftLabel("Поріг виявлення, дБ");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderInt("##scanner3_level_3", &_this->curr_level, -150, 0))
        {
            _this->curr_level = std::clamp<int>(_this->curr_level, -150, -30);
            _this->glevel = _this->curr_level;

            gui::mainWindow.setLevelDbScan(currSrvr, _this->glevel);
            gui::mainWindow.setUpdateMenuSnd6Scan(currSrvr, true);
        }
        //-----------------------------------------------------------------
        // ImGui::Checkbox("Призупиняти сканування при виявленні сигналу##_status_scanner3", &_this->status_stop); // status_stop

        //---------------------------------------------------
        // if (!_this->status_stop) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Час очікування, сек");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##linger_timeWait_scanner_3", &_this->maxRecWaitTime, 1, _this->maxRecDuration - 1))
        {
            _this->maxRecWaitTime = std::clamp<int>(_this->maxRecWaitTime, 1, _this->lingerTime - 1);
            config.acquire();
            config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
            config.release(true);
            gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, _this->maxRecWaitTime);
            gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, _this->lingerTime);
            gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
        }
        ImGui::LeftLabel("Макс. тривалість запису, сек");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##maxRecDuration_scanner_3", &_this->lingerTime, 1, _this->maxRecDuration - 1))
        { // _waitingTime
            // flog::info("_this->lingerTime {0}, _this->maxRecWaitTime {1}", _this->lingerTime, _this->maxRecWaitTime);
            _this->lingerTime = std::clamp<int>(_this->lingerTime, 1, _this->maxRecDuration - 1);
            _this->maxRecWaitTime = std::clamp<int>(_this->maxRecWaitTime, 1, _this->lingerTime - 1);
            // flog::info("_this->lingerTime {0}, _this->maxRecWaitTime {1}", _this->lingerTime, _this->maxRecWaitTime);
            config.acquire();
            config.conf["lingerTime"] = _this->lingerTime;
            config.conf["maxRecWaitTime"] = _this->maxRecWaitTime;
            config.release(true);
            gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, _this->lingerTime);
            gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, _this->maxRecWaitTime);
            gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
        }
        // if (!_this->status_stop) { ImGui::EndDisabled(); }
        //<---------------------------------------------------
        if (NullBank)
        {
            style::endDisabled();
        }
        //---------------------------------------------------

        if (_run || _work > 0 || showThisMenu)
        {
            ImGui::EndDisabled();
        }
        bool _empty = NullBank; // ((_this->bookmarks_size == 0) ? true : false);
        bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();

        if (_this->currSource == SOURCE_ARM)
        {
            uint8_t currSrv = gui::mainWindow.getCurrServer();
            _this->running.store(gui::mainWindow.getbutton_scan(currSrv));
            _air_recording = gui::mainWindow.isPlaying();
            bool rec_work = gui::mainWindow.getServerRecording(currSrv);

            int _control = gui::mainWindow.getServerStatus(currSrv);
            // flog::info("_this->running {0}, _air_recording {1}, _control {2}, _work {3}, rec_work {4}, _ifStartElseBtn {5}", _this->running, _air_recording, _control, _work, rec_work, _ifStartElseBtn);
            if (_control != ARM_STATUS_FULL_CONTROL)
            {
                ImGui::BeginDisabled();
                ImGui::Button("СТАРТ##scanner3_arm_start_1", ImVec2(menuWidth, 0));
                ImGui::EndDisabled();
            }
            else
            {
                if (!_this->running.load())
                {
                    if (_work > 0 || rec_work > 0 || _empty || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::BeginDisabled();
                    if (ImGui::Button("СТАРТ##scanner3_arm_start_2", ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setbutton_scan(currSrv, true);
                        gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, _this->maxRecWaitTime);
                        gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, _this->lingerTime);
                        // gui::mainWindow.setLevelDbScan(currSrvr, _this->level);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                    }
                    if (_work > 0 || rec_work > 0 || _empty || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::EndDisabled();
                }
                else
                {
                    if (ImGui::Button("СТОП ##scanner3_arm_start_3", ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setbutton_scan(currSrv, false);
                        gui::mainWindow.setMaxRecWaitTime_scan(MAX_SERVERS, _this->maxRecWaitTime);
                        gui::mainWindow.setMaxRecDuration_scan(MAX_SERVERS, _this->lingerTime);
                        // gui::mainWindow.setLevelDbScan(currSrvr, _this->level);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                    }
                }
            }
        }
        else
        {
            core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
            if (!_this->running.load() && !_this->_detected)
            {
                if (_work > 0 || _empty || (_this->isServer && _ifStartElseBtn))
                    ImGui::BeginDisabled();
                core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
                if (_air_recording == 1 && _count_freq > 0)
                {
                    if (ImGui::Button("СТАРТ##scanner3_start_3", ImVec2(menuWidth, 0)))
                    {
                        _this->start();
                    }
                }
                else
                {
                    style::beginDisabled();
                    ImGui::Button("СТАРТ##scanner3_start_3", ImVec2(menuWidth, 0));
                    style::endDisabled();
                }
                if (_work > 0 || _empty || (_this->isServer && _ifStartElseBtn))
                    ImGui::EndDisabled();
            }
            else
            {
                if (_air_recording == 0)
                {
                    _this->stop();
                    _this->_detected.store(false);
                }

                if (_this->_detected == true && _this->_recording.load() && _this->status_stop == true)
                { // && _this->status_stop == true
                    if (ImGui::Button("ДАЛІ##scanner3_cans_3", ImVec2(menuWidth, 0)))
                    {
                        _this->stop();
                        _this->_recording.store(false);
                        _this->_Receiving = false;
                        _this->start();
                    }
                }
                else
                {
                    if (ImGui::Button("СТОП ##scanner3_start_3", ImVec2(menuWidth, 0)))
                    {
                        _this->stop();
                        _this->_detected.store(false);
                    }
                }
            }

            if (_this->_detected)
            {
                if (_this->_recording.load() && _this->status_stop == true)
                { //
                    if (ImGui::Button("Зупинити сканування##scanner3_recstop_3", ImVec2(menuWidth, 0)))
                    {
                        _this->stop();
                        _this->_recording.store(false);
                        _this->_Receiving = false;
                        _this->_detected.store(false);
                    }
                }
                else
                {
                    if (ImGui::Button("ДАЛІ##scanner3_cans_3", ImVec2(menuWidth, 0)))
                    {
                        _this->stop();
                        _this->_recording.store(false);
                        _this->_Receiving = false;
                        _this->start();
                    }
                }
            }
            if (!_this->running.load() && !_this->_detected)
            {
                ImGui::Text("Статус: Неактивний");
            }
            else
            {
                if (_this->_recording.load())
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", "Статус: Реєстрація");
                }
                else if (_this->tuning)
                {
                    ImGui::TextColored(ImVec4(0, 1, 1, 1), "%s", "Статус: Сканування");
                }
                else if (_this->_detected)
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", "Статус: Приймання");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0, 1, 1, 1), "%s", "Статус: Сканування");
                }
            }
        }

        //// if (_this->selectedListName == "") { style::endDisabled(); }

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

        if (_this->menuListOpen)
        {
            _this->menuListOpen = _this->menuDialog();
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
                    if (_this->importBookmarks(paths[0], _this->AddImport))
                    {
                        // _this->loadByName(_this->selectedListName);
                        // _this->refreshWaterfallBookmarks(false);
                        // uint8_t currSrvr = gui::mainWindow.getCurrServer();
                        _this->getScanLists();
                        flog::info("777");
                        _this->selectedListId = std::distance(_this->listNames.begin(), std::find(_this->listNames.begin(), _this->listNames.end(), _this->selectedListName));
                        flog::info("!!!! getScanLists() {0}, selectedListName {1}", _this->selectedListId, _this->selectedListName);
                        _this->curr_listName = _this->selectedListName;
                        _this->bookmarks_size = _this->bookmarks.size();
                        gui::mainWindow.setScanListNamesTxt(_this->listNamesTxt);
                        gui::mainWindow.setidOfList_scan(currSrvr, _this->selectedListId);
                        gui::mainWindow.setUpdateModule_scan(currSrvr, true);
                        gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateListRcv6Scan(currSrvr, true);
                        gui::mainWindow.setUpdateScanListForBotton(currSrvr, true);
                    }
                }
                catch (const std::exception &e)
                {
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
            _this->AddImport = false;
            _this->_error = false;
        };

        if (_this->exportOpen)
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
    void findMinMaxFreqBw(const std::map<std::string, ObservationBookmark> &bookmarks,
                          double &minFreq,
                          double &maxFreq,
                          float &minBw,
                          float &maxBw)
    {

        minFreq = std::numeric_limits<double>::max();
        maxFreq = std::numeric_limits<double>::lowest();
        minBw = std::numeric_limits<float>::max();
        maxBw = std::numeric_limits<float>::lowest();

        for (const auto &[key, bookmark] : bookmarks)
        {
            if (bookmark.frequency < minFreq)
                minFreq = bookmark.frequency;
            if (bookmark.frequency > maxFreq)
                maxFreq = bookmark.frequency;

            if (bookmark.bandwidth < minBw)
                minBw = bookmark.bandwidth;
            if (bookmark.bandwidth > maxBw)
                maxBw = bookmark.bandwidth;
        }
    }

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
            if (mid > 0)
            {
                // std::accumulate возвращает double, если начальное значение 0.0, что хорошо
                noiseLevel = std::accumulate(levels.begin(), levels.begin() + mid, 0.0) / mid;
            }
            else if (!levels.empty()) // Если mid=0, но вектор не пуст (т.е. size=1)
            {
                noiseLevel = levels[0];
            }
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
        }

        // Вычисление среднего уровня шума (игнорируем поддиапазоны с сильным сигналом)
        double avgNoiseLevel = 0.0;
        int validNoiseCount = 0;
        for (double dlevel : noiseLevels)
        {
            if (dlevel > -150.0)
            { // Учитываем только поддиапазоны с реальным шумом
                avgNoiseLevel += dlevel;
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

    /*
     * @brief Выполняет полную калибровку порогов для всех закладок.
     * Эта функция должна вызываться только при старте сканера, так как она
     * выполняет длительные операции перестройки тюнера.
     * @param thresholdMargin Насколько дБ выше шума устанавливать порог.
     */
    void setupAutoThresholdsWorker(float thresholdMargin)
    {
        flog::info("Starting automatic threshold calculation...");
        bool anyError = false;
        double totalThresholdSum = 0.0;
        int calculationCount = 0;
        int globalLevel = 0;

        for (auto &[key, bm] : bookmarks)
        {
            double freq = bm.frequency;
            float bw = bm.bandwidth;

            flog::info("Tuning to {0} Hz for threshold calculation", freq);
            tuner::centerTuning(gui::waterfall.selectedVFO, freq);
            tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, freq);
            std::this_thread::sleep_for(std::chrono::milliseconds(TUNING_SETTLE_TIME_MS + 150));

            int dataWidth = 0;
            float *data = gui::waterfall.acquireLatestFFT(dataWidth);
            if (!data || dataWidth <= 0)
            {
                flog::error("Failed to acquire FFT data for {0} Hz", freq);
                bm.level = glevel;
                anyError = true;
                if (data)
                    gui::waterfall.releaseLatestFFT();
                continue;
            }

            double centerFreq = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
            double wfWidth = gui::waterfall.getViewBandwidth();
            double wfStart = centerFreq - wfWidth / 2.0;

            std::vector<float> noiseLevels;

            // ======================= ИЗМЕНЕНИЕ №1 =======================
            // Отступаем на полную ширину полосы + 25% запас, чтобы не попасть в соседний канал
            const float offsetMultiplier = 1.25f;

            // ======================= ИЗМЕНЕНИЕ №2 =======================
            // Адаптивное количество замеров в зависимости от полосы
            int noiseSamplesPerSide = (bw <= 12500.0f) ? 3 : (bw <= 100000.0f ? 2 : 1);
            // ============================================================

            for (int side = -1; side <= 1; side += 2)
            {
                // Используем 'noiseSamplesPerSide' в цикле
                for (int i = 1; i <= noiseSamplesPerSide; ++i)
                {
                    // Используем 'offsetMultiplier' для шага
                    double noiseFreq = freq + side * i * (bw * offsetMultiplier);
                    LevelInfo info = getMaxLevelNew(data, noiseFreq, bw, dataWidth, wfStart, wfWidth);

                    // Ваша оригинальная проверка шума (с исправленным синтаксисом)
                    float delta = info.maxLevel - info.noiseLevel;
                    flog::warn("(info.maxLevel {0} - info.noiseLevel {1} = {2}", info.maxLevel, info.noiseLevel, delta);
                    // if (delta < 10.0f)
                    if (20.0f > (delta) < 2.0f)
                    {
                        noiseLevels.push_back(info.noiseLevel);
                    }
                }
            }

            gui::waterfall.releaseLatestFFT();
            int currentFreqThreshold = globalLevel;

            if (!noiseLevels.empty())
            {
                float noiseFloor = *std::min_element(noiseLevels.begin(), noiseLevels.end());
                int newThreshold = static_cast<int>(noiseFloor + thresholdMargin);
                bm.level = newThreshold;
                flog::warn("Threshold for '{0}': {1} dB (Noise floor: {2:.1f} dB + {3:.1f} dB)", key.c_str(), newThreshold, noiseFloor, thresholdMargin);
                currentFreqThreshold = newThreshold; // Используем 'newThreshold', а не пересчитываем
            }
            else
            {
                flog::warn("Could not find clean noise for '{0}'. Using default threshold: {1} dB", key.c_str(), globalLevel);
                if (globalLevel == 0)
                {
                    bm.level = glevel;
                }
                else
                    bm.level = globalLevel;
                anyError = true;
            }

            // Ваш оригинальный подсчет среднего
            totalThresholdSum += currentFreqThreshold;
            calculationCount++;
            if (calculationCount > 0)
            { // Защита от деления на ноль
                globalLevel = static_cast<int>(totalThresholdSum / calculationCount);
            }
        }

        if (anyError)
        {
            // _error = true;
            // txt_error = "Не для всех частот удалось рассчитать порог.";
        }

        flog::info("Automatic threshold calculation finished.");
    }

    //=====================================================================================================
    //=====================================================================================================
    void start()
    {
        if (isARM)
            return;
        if (running.load())
            return;
        int _air_recording;
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
        flog::info("AIR Recording is '{0}'", _air_recording);
        if (_air_recording == 0)
        {
            gui::mainWindow.setbutton_scan(gui::mainWindow.getCurrServer(), false);
            return;
        }
        bookmarks_size = bookmarks.size();
        if (bookmarks_size == 0)
        {
            flog::warn("bookmarks is emply!");
            gui::mainWindow.setbutton_scan(gui::mainWindow.getCurrServer(), false);
            return;
        }
        if (_detected == false)
            itbook = bookmarks.begin();
        _detected.store(false);
        if (itbook == bookmarks.end())
        {
            gui::mainWindow.setbutton_scan(gui::mainWindow.getCurrServer(), false);
            flog::warn("bookmarks is emply 2!");
            return;
        }

        flog::warn("status_auto_level3 {0}!", status_auto_level3);

        if (status_auto_level3)
        {
            const float threshold_margin = 9.0f;
            setupAutoThresholdsWorker(threshold_margin);
        }
        else
        {
            curr_level = glevel;
            gui::mainWindow.setLevelDbScan(CurrSrvr, glevel);
        }
        gui::mainWindow.setChangeGainFalse();

        running.store(true);
        _recording.store(false);
        restart_requested.store(false);

        core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);

        std::string folderPath = "%ROOT%/recordings";
        // expandedLogPath = expandString(folderPath + genLogFileName("/freqmgr_"));

        cnt_skip = 0;
        radioMode = 0;
        if (radioMode == 0)
        {
            status_direction = true;
        }
        else
        {
            status_direction = true; // false;
        }

        auto bm = itbook->second;
        if (!gui::waterfall.selectedVFO.empty())
        {
            /// std::lock_guard<std::mutex> lck(tuneMtx);
            tuner::centerTuning(gui::waterfall.selectedVFO, bm.frequency);
            tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, bm.frequency);
            gui::waterfall.centerFreqMoved = true;
            gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        }

        {
            double bw = 0.922;
            gui::mainWindow.setViewBandwidthSlider(0.922);
            double factor = bw * bw;
            if (factor > 0.85)
                factor = 0.85;
            double wfBw = gui::waterfall.getBandwidth();
            double delta = wfBw - 1000.0;
            double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);
            // flog::info("bw3 {0}, finalBw {1}, wfBw {2}, delta {3}, factor {4}", bw, finalBw, wfBw, delta, factor);
            if (finalBw > VIEWBANDWICH)
                finalBw = VIEWBANDWICH;
            gui::waterfall.setViewBandwidth(finalBw);
            gui::waterfall.setViewOffset(gui::waterfall.vfos["Канал приймання"]->centerOffset); // center vfo on screen
        }
        // tuner::tune(tuningMode, gui::waterfall.selectedVFO, bm.frequency);
        flog::info("start() Scan3(), running={0},  bm.frequency {1}", running.load(), bm.frequency);
        gui::mainWindow.setbutton_scan(gui::mainWindow.getCurrServer(), true);
        gui::mainWindow.setidOfList_scan(gui::mainWindow.getCurrServer(), selectedListId);
        gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);

        current = bm.frequency;
        // 2. Имитируем, что мы только что переключились и ждем стабилизации
        tuning = true;
        _Receiving = false;
        lastTuneTime = std::chrono::high_resolution_clock::now();

        workerThread = std::thread(&ObservationManagerModule::worker, this);
    }

    void stop()
    {
        flog::info("void stop(), running={0}", running.load());
        running.store(false);
        gui::mainWindow.setbutton_scan(gui::mainWindow.getCurrServer(), false);
        gui::mainWindow.setUpdateMenuSnd6Scan(MAX_SERVERS, true);

        if (_recording.load())
        {
            flog::warn("STOP Receiving!");
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
        }
        _recording.store(false);
        core::configManager.acquire();
        core::configManager.conf["frequency"] = current;
        core::configManager.release(true);

        flog::info("void stop(), running={0}", running.load());

        usleep(1000);
        running.store(false);
        _recording.store(false);

        if (workerThread.joinable())
        {
            workerThread.join();
        }
        if (curr_nameWavFile != "") //  && radioMode == 0
            curlPOST_end(curr_nameWavFile);
        core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);
    }

    void worker()
    {
        auto zirotime = std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::seconds(1));
        auto init_level_time = std::chrono::high_resolution_clock::now(); // std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::seconds(1));
        firstSignalTime = zirotime;
        // 10Hz scan loop
        // _Receiving = false;
        // tuning = false;
        _detected.store(false);

        name = itbook->first;
        auto bm = itbook->second;
        itbook->second.selected = true;
        applyMode(bm, gui::waterfall.selectedVFO);

        // flog::error("bm.frequency {0}\n", bm.frequency);

        while (running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scan3Mtx);
                auto now = std::chrono::high_resolution_clock::now();

                // Enforce tuning
                if (gui::waterfall.selectedVFO.empty())
                {
                    running.store(false);
                    return;
                }

                double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
                double wfWidth = gui::waterfall.getViewBandwidth();
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);

                double curr_freq = gui::waterfall.getCenterFrequency(); //  + gui::waterfall.vfos["Канал приймання"]->generalOffset;

                if (current != curr_freq)
                {
                    gui::waterfall.centerFreqMoved = false;
                    // flog::info("wfCenter {0}. Setting  bm.frequency {1} != curr_freq {2}...", wfCenter, current, gui::waterfall.getCenterFrequency());
                    if (!gui::waterfall.selectedVFO.empty())
                    {
                        // std::lock_guard<std::mutex> lck(tuneMtx);
                        flog::warn("SETTING tuner::centerTuning bm.frequency {0}, old curr_freq {1}", current, curr_freq);
                        tuner::centerTuning(gui::waterfall.selectedVFO, current);
                        tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, current);
                    }
                    else
                    {
                        flog::error("gui::waterfall.selectedVFO.empty() bm.frequency {0}", current);
                    }

                    gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
                    gui::waterfall.centerFreqMoved = true;

                    if (wfStart < current || current > wfEnd)
                    {
                        flog::warn("wfStart {0} < || bm.frequency {1} ||  > wfEnd {2}. Waiting 1s for SDR to settle...", wfStart, current, wfEnd);
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }

                if (tuning && _Receiving == false)
                {
                    // flog::warn("Tuning");
                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime)).count() > TUNING_SETTLE_TIME_MS)
                    {
                        tuning = false;
                        _Receiving = true;
                        firstSignalTime = zirotime;
                    }
                    else
                    {
                        continue;
                    }
                }

                // Get FFT data
                int dataWidth = 0;
                float *data = gui::waterfall.acquireLatestFFT(dataWidth);
                if (!data)
                {
                    continue;
                }

                // Get gather waterfall data
                wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
                wfWidth = gui::waterfall.getViewBandwidth();
                wfStart = wfCenter - (wfWidth / 2.0);
                wfEnd = wfCenter + (wfWidth / 2.0);
                // Gather VFO data
                double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

                // Check if we are waiting for a tune
                if (_Receiving)
                {
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    // flog::info("Check Freq: {0} Hz. Max Level: {1} dB. Threshold: {2} dB", current, maxLevel, level + 2);
                    curr_level = glevel;
                    if (status_auto_level3)
                    {
                        curr_level = bm.level;
                        gui::mainWindow.setLevelDbScan(CurrSrvr, curr_level);
                    }
                    if (maxLevel > curr_level)
                    {
                        if (firstSignalTime == zirotime)
                        {
                            firstSignalTime = now;
                            int _mode = _mode = gui::mainWindow.getselectedDemodID();
                            // core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                            // curr_nameWavFile = genWavFileName(current, _mode);
                            std::string templ = "$y$M$d-$u-$f-$b-$n-$e.wav";
                            // if (radioMode == 0) {
                            curr_nameWavFile = genWavFileName(templ, current, _mode);
                            if (curr_nameWavFile != "")
                                curlPOST_begin(curr_nameWavFile);

                            if (_record == true && running.load() && !_recording.load())
                            {
                                flog::info("TRACE. START Receiving! curr_level = {0}, maxLevel = {1}, current = {2} !", glevel, maxLevel, current);
                                int recMode = 1; // RECORDER_MODE_AUDIO;
                                // if (_mode == 7)  // TAW
                                //    recMode = 0; // RECORDER_MODE_BASEBAND
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START, (void *)curr_nameWavFile.c_str(), NULL);
                                _recording.store(true);
                            }
                            else
                            {
                                flog::info("TRACE. curr_level = {0}, maxLevel = {1}, current = {2} !", glevel, maxLevel, current);
                            }
                            flog::info("TRACE. START Receiving... curr_level = {0}, maxLevel = {1}, current = {2},  _recording={3}, _record={4} !", glevel, maxLevel, current, _recording.load(), _record);
                        }
                        else
                        {
                        }
                        lastSignalTime = now;
                        _detected.store(true);
                        // if (status_stop) {
                        if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSignalTime)).count() > lingerTime * 1000)
                        {
                            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
                            _recording.store(false);
                            _Receiving = false;
                            _detected.store(false);
                        }
                        // }
                    }
                    else
                    {
                        if (_detected == true)
                        {
                            if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > maxRecWaitTime * 1000)
                            {
                                if (_recording.load())
                                    core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
                                _recording.store(false);
                                _Receiving = false;
                                _detected.store(false);
                                tuning = false;
                            }
                        }
                        else
                        {
                            _Receiving = false;
                            tuning = false;
                        }
                    }
                    if (_Receiving == false)
                    {
                        if (curr_nameWavFile != "")
                        {
                            curlPOST_end(curr_nameWavFile);
                            curr_nameWavFile = "";
                        }
                    }
                }

                if (!_Receiving && !tuning)
                {
                    //                    flog::warn("Seeking signal");
                    itbook = next(itbook);
                    if (itbook == bookmarks.end())
                    {
                        itbook = bookmarks.begin();
                    }
                    name = itbook->first;
                    bm = itbook->second;

                    // gui::waterfall.centerFreqMoved = false;
                    // std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    current = bm.frequency;
                    flog::warn("Preparing to tune to new frequency {0}", current);
                    gui::waterfall.releaseLatestFFT();
                    applyMode(bm, gui::waterfall.selectedVFO);

                    /// flog::info("Set freq! bm[frequency] = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);

                    if (gui::waterfall.selectedVFO.empty())
                    {
                        running.store(false);
                        return;
                    }

                    tuning = true;
                    lastTuneTime = std::chrono::high_resolution_clock::now();
                    _detected.store(false);

                    /*
                    double curr_freq = gui::waterfall.getCenterFrequency(); //  + gui::waterfall.vfos["Канал приймання"]->generalOffset;

                    if (bm.frequency != curr_freq)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100))
                        flog::info("wfCenter {0}. Setting  bm.frequency {1} != curr_freq {2}...", wfCenter, bm.frequency, curr_freq);
                        if (!gui::waterfall.selectedVFO.empty())
                        {
                            // std::lock_guard<std::mutex> lck(tuneMtx);
                            tuner::centerTuning(gui::waterfall.selectedVFO, bm.frequency);
                            tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, bm.frequency);
                        }
                        // gui::waterfall.centerFreqMoved = false;

                        // flog::error("SET tuner::centerTuning bm.frequency {0}", bm.frequency);
                        // tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, bm.frequency);
                        gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
                        gui::waterfall.centerFreqMoved = true;
                    */
                    /*
                    if (wfStart < bm.frequency || bm.frequency > wfEnd)
                    {
                        flog::warn("wfStart {0} < || bm.frequency {1} ||  > wfEnd {2}. Waiting 1s for SDR to settle...", wfStart, bm.frequency, wfEnd);
                        std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    }
                    */
                    // }
                    /*
                    lastTuneTime = std::chrono::high_resolution_clock::now();
                    tuning = true;
                    _detected.store(false);
                    */
                    //====================================================
                    if (status_auto_level3)
                    {
                        bool new_gain = gui::mainWindow.getChangeGain();
                        if (cnt_skip >= COUNT_FOR_REFIND_SKIP)
                        {
                            restart_requested.store(true);
                            flog::info("REFIND_LEVEL cnt_skip ={0}", cnt_skip);
                        }
                        else if (new_gain)
                        {
                            flog::info("new_gain {0}", new_gain);
                            gui::mainWindow.setChangeGainFalse();
                            trigger_time = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                            timer_started = true;
                        }
                        else if (timer_started && std::chrono::steady_clock::now() >= trigger_time)
                        {
                            restart_requested.store(true);
                            timer_started = false;
                        }
                        else if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() > INTERVAL_FOR_FIND_Threshold * 60000)
                        {
                            flog::info("time = {0}, (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count() {1} ", (std::chrono::duration_cast<std::chrono::milliseconds>(now - init_level_time)).count(), INTERVAL_FOR_FIND_Threshold * 60000);
                            restart_requested.store(true);
                        }
                    }
                }

                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
            }
        }
        flog::info("record = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);
    }

    bool findSignal(bool scanDir, double &bottomLimit, double &topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float *data, int dataWidth)
    {
        bool found = false;
        double freq = current;
        // Check signal cure_level
        if (!_recording.load())
        {
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            if (maxLevel >= (float)curr_level)
            {
                found = true;
                _Receiving = true;
                current = freq;
            }
        }
        return found;
    }

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

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void *ctx)
    {
        ObservationManagerModule *_this = (ObservationManagerModule *)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF)
        {
            return;
        }

        if (!_this->infoThreadStarted.load())
        {
            if (_this->isServer || _this->isARM)
            {
                _this->workerInfoThread = std::thread(&ObservationManagerModule::workerInfo, _this);
                _this->infoThreadStarted.store(true);
            }
        }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP)
        {
            for (auto const bm : _this->waterfallBookmarks)
            {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                // flog::error("TOP bm.bookmarkName.c_str() ={0}", bm.bookmarkName.c_str());

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq)
                {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.min.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0)
                {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x)
                {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.min.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM)
        {
            for (auto const bm : _this->waterfallBookmarks)
            {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq)
                {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }
                // flog::error("BOTTOM bm.bookmarkName.c_str() ={0}", bm.bookmarkName.c_str());
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.max.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0)
                {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x)
                {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.max.y - nameSize.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void *ctx)
    {
        ObservationManagerModule *_this = (ObservationManagerModule *)ctx;
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
        return;
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
        ImGui::Text("Частота: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Ширина Смуги: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Тип сигналу: %s", std::to_string(hoveredBookmark.bookmark.Signal).c_str());
        // ImGui::Text("Поріг виявлення: %s", utils::formatFreq(hoveredBookmark.bookmark.level).c_str());
        ImGui::Text("Вид демод.: %s", demodModeList[hoveredBookmark.bookmark.mode]);

        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    bool AddImport = false;
    pfd::open_file *importDialog;
    pfd::save_file *exportDialog;

    bool importBookmarks(std::string path, bool add)
    {
        if (add)
        {
            importBookmarks_add(path);
            return true;
        }
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

        if (!importBookmarks["rx-mode"].is_string())
        {
            flog::error("Bookmark attribute is invalid ('rx-mode' not is_object)");
            // return false;
        }
        if (importBookmarks["rx-mode"] != "scanning")
        {
            flog::error("Bookmark attribute is invalid ('rx-mode' must have the name 'scanning' {0})", importBookmarks["rx-mode"]);
            return false;
        }

        std::string NameList = "";
        for (auto const [_name, bm] : importBookmarks.items())
        {
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
        newList["bookmarks"] = json::object();
        config.conf["lists"][NameList] = newList;
        config.conf["selectedList"] = NameList;
        config.conf["maxRecWaitTime"] = maxRecWaitTime;
        config.conf["lingerTime"] = lingerTime;
        config.release(true);

        refreshLists();
        selectedListName = NameList;
        loadByName(selectedListName);
        // Load every bookmark
        for (auto const [_name, bm] : importBookmarks[NameList]["bookmarks"].items())
        {
            if (bookmarks.find(_name) != bookmarks.end())
            {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.level = bm["level"];
            fbm.selected = false;
            try
            {
                fbm.Signal = bm["Signal"];
            }
            catch (const std::exception &e)
            {
                fbm.Signal = 0;
                std::cerr << e.what() << '\n';
            }

            bookmarks[_name] = fbm;
        }
        saveByName(selectedListName);
        fs.close();
        return true;
    }

    bool importBookmarks_add(std::string path)
    {
        // std::ifstream fs(path);
        // json importBookmarks;
        // fs >> importBookmarks;
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

        if (importBookmarks["rx-mode"] != "scanning" || !importBookmarks["rx-mode"].is_string())
        {
            flog::error("Bookmark attribute is invalid ('rx-mode' must have the name 'scanning')");
            return false;
        }
        std::string NameList = selectedListName;
        std::string currNameList = "";

        flog::info("importBookmarks NameList = {0} ", NameList);
        for (auto const [_name, bm] : importBookmarks.items())
        {
            if (_name != "InstNum" && _name != "bank-name" && _name != "domain" && _name != "rx-mode" && _name != "time_created")
            {
                currNameList = std::string(_name);
                break;
            }
        }
        if (!importBookmarks[currNameList].contains("bookmarks"))
        {
            flog::error("File does not contains any bookmarks");
            return false;
        }

        if (!importBookmarks[currNameList]["bookmarks"].is_object())
        {
            flog::error("Bookmark attribute is invalid");
            return false;
        }

        json newList = json({});

        //        refreshLists();
        //        selectedListName = NameList;
        //        loadByName(selectedListName);
        // Load every bookmark
        int ibmName = 0;
        for (int i = 1; i < 1000; i++)
        {
            std::string bmName = std::to_string(i);
            if (bookmarks.find(bmName) == bookmarks.end())
            {
                ibmName = i;
                break;
            }
        }
        if (ibmName == 0)
            ibmName = 1;
        // flog::info("ibmName {0}", ibmName);
        for (auto const [_name, bm] : importBookmarks[currNameList]["bookmarks"].items())
        {
            std::string bmName = std::to_string(ibmName);
            if (bookmarks.find(bmName) != bookmarks.end())
            {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.level = bm["level"];
            try
            {
                fbm.Signal = bm["Signal"];
            }
            catch (const std::exception &e)
            {
                fbm.Signal = 0;
                std::cerr << e.what() << '\n';
            }
            // flog::info("NameList {0} , bmName {1}, fbm.frequency {2}", NameList, bmName, fbm.frequency);
            fbm.selected = false;

            bookmarks[bmName] = fbm;
            ibmName++;
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
        ObservationManagerModule *_this = (ObservationManagerModule *)ctx;
        flog::info("moduleInterfaceHandler, name = {0}", _this->selectedListName);

        struct FreqData
        {
            int freq;
            int mode;
            int level;
            int Signal;
        } pFreqData;
        pFreqData = *(static_cast<FreqData *>(in));
        // pFreqData.freq = _this->current;
        int _mode = pFreqData.mode; //  *(int*)in;
        if (gui::waterfall.selectedVFO == "")
        {
            _this->editedBookmark.frequency = pFreqData.freq;
            _this->editedBookmark.bandwidth = 0;
            _this->editedBookmark.mode = (int)_mode;
        }
        else
        {
            _this->editedBookmark.frequency = pFreqData.freq;
            _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            _this->editedBookmark.mode = (int)_mode;
            /*
            if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                int mode = gui::mainWindow.getselectedDemodID();
             //   core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                _this->editedBookmark.mode = mode;
            }
            */
        }

        _this->editedBookmark.level = pFreqData.level;   // _this->level;
        _this->editedBookmark.Signal = pFreqData.Signal; // _this->level;

        _this->editedBookmark.selected = false;

        //            _this->createOpen = true;

        // Find new unique default name
        if (_this->bookmarks.find("1") == _this->bookmarks.end())
        {
            _this->editedBookmarkName = "1";
        }
        else
        {
            char buf[32];
            for (int i = 1; i < 1000; i++)
            {
                sprintf(buf, "%d", i);
                if (_this->bookmarks.find(buf) == _this->bookmarks.end())
                {
                    break;
                }
            }
            _this->editedBookmarkName = _this->removeSpecialChars(buf);
        }

        // If editing, delete the original one
        //              if (editOpen) {
        _this->bookmarks.erase(_this->firstEditedBookmarkName);
        //                }
        _this->bookmarks[_this->editedBookmarkName] = _this->editedBookmark;

        _this->saveByName(_this->selectedListName);
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
    bool menuListOpen = false;
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
    bool status_stop = true;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;

    int bookmarkDisplayMode = 0;

    // bool running.store(false);
    // bool _recording.store(false);
    // bool _detected.store(false);

    std::atomic<bool> running = false;
    std::atomic<bool> _recording = false;
    std::atomic<bool> _detected = false;

    std::thread workerThread;
    std::mutex scan3Mtx;

    double current = 88000000.0;
    double passbandRatio = 10.0;
    static constexpr int TUNING_SETTLE_TIME_MS = 250;
    int lingerTime = 5;
    int maxRecDuration = 60;
    int glevel = -50;
    int curr_level = -50;
    bool _Receiving = true;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool _record = true;
    int _recordTime = 3;
    std::ofstream logfile;
    std::string root = (std::string)core::args["root"];

    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;

    // std::string expandedLogPath;
    std::string curr_listName = "";

    std::string curr_nameWavFile = "";

    std::vector<uint32_t> bandwidthsList;
    double _frec = 1000000.0;
    int _bandwidthId = 0;
    bool _raw = false;
    int NumIDBookmark = 0;

    std::string txt_error = "";
    bool _error = false;

    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";

    int radioMode = 0;
    bool status_direction = false;
    int maxRecWaitTime = 10;

    std::thread workerInfoThread;
    std::atomic<bool> infoThreadStarted = false;
    bool isARM = false;
    bool isServer = false;
    std::string currSource;
    uint8_t CurrSrvr;
    bool Admin = false;
    bool use_curl = false;
    double sigmentLeft, sigmentRight;
    bool status_auto_level3 = false;
    bool timer_started = false;
    std::chrono::steady_clock::time_point trigger_time;
    const double scan_band = 12500.0; // Шаг сканирования в Гц
    int cnt_skip = 0;
    std::atomic<bool> restart_requested = false;
    // std::mutex tuneMtx;
};

MOD_EXPORT void _INIT_()
{
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["maxRecWaitTime"] = 10;
    def["lingerTime"] = 5;
    def["glevel"] = -70;

    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/frequency_manager_config.json");
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
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name)
{
    return new ObservationManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance)
{
    delete (ObservationManagerModule *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
}
