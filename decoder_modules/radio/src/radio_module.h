#pragma once
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <dsp/chain.h>
#include <dsp/noise_reduction/noise_blanker.h>
#include <dsp/noise_reduction/fm_if.h>
#include <dsp/noise_reduction/squelch.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/filter/deephasis.h>
#include <core.h>
#include <stdint.h>
#include <utils/optionlist.h>
#include "radio_interface.h"
#include "demod.h"
#include "core.h"
#include <gui/menus/source.h>

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())
#define MIN_BBand 100000
#define MAX_BBand 8500000
#define IQ false
#define MODE_AZALIY 2

std::map<DeemphasisMode, double> deempTaus = {
    {DEEMP_MODE_22US, 22e-6},
    {DEEMP_MODE_50US, 50e-6},
    {DEEMP_MODE_75US, 75e-6}};

std::map<IFNRPreset, double> ifnrTaps = {
    {IFNR_PRESET_NOAA_APT, 9},
    {IFNR_PRESET_VOICE, 15},
    {IFNR_PRESET_NARROW_BAND, 31},
    {IFNR_PRESET_BROADCAST, 32}};

class RadioModule : public ModuleManager::Instance
{
public:
    RadioModule(std::string name)
    {
        this->name = name;

        flog::info("start constructor RadioModule");

        // Initialize option lists
        deempModes.define("Вимкнен.", DEEMP_MODE_NONE);
        deempModes.define("22мкс", DEEMP_MODE_22US);
        deempModes.define("50мкс", DEEMP_MODE_50US);
        deempModes.define("75мкс", DEEMP_MODE_75US);

        ifnrPresets.define("NOAA APT", IFNR_PRESET_NOAA_APT);
        ifnrPresets.define("Мова", IFNR_PRESET_VOICE);
        ifnrPresets.define("Narrow Band", IFNR_PRESET_NARROW_BAND);

        // Initialize the config if it doesn't exist
        bool created = false;
        config.acquire();
        if (!config.conf.contains(name))
        {
            config.conf[name]["selectedDemodId"] = 1;
            created = true;
        }
        selectedDemodID = config.conf[name]["selectedDemodId"];
        config.release(created);

        core::configManager.acquire();
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
            radioMode = (int)core::configManager.conf["RadioMode"];
            source = core::configManager.conf["source"];
        }
        catch (const std::exception &e)
        {
            radioMode = 0;
        }

        core::configManager.release();
        clnt_mode = -1; //  || source == "Airspy"

        if (source == "Азалія-сервер")
        {
            clnt_mode = 0;
            if (selectedDemodID != RADIO_DEMOD_WFM)
            {
                selectedDemodID = RADIO_DEMOD_WFM;
                config.acquire();
                config.conf[name]["selectedDemodId"] = selectedDemodID;
                config.release(true);
            }
        }
        else if (source == "Азалія-клієнт")
        {
            clnt_mode = 1;
            if (selectedDemodID != RADIO_DEMOD_USB)
            {
                selectedDemodID = RADIO_DEMOD_USB;
                config.acquire();
                config.conf[name]["selectedDemodId"] = selectedDemodID;
                config.release(true);
            }
        }
        flog::info("[RADIO Module] source = {0}, clnt_mode = {1}, bandwidth {2}, selectedDemodID {3}!", source, clnt_mode, bandwidth, selectedDemodID);

        // " 1\0 2.1\0 4\0 6.25\0 12.5\0 25\0 50\0 100\0 220\0";
        // 6,25; 2.1. 12,5; 25; 220
        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(2700);
        bandwidthsList.push_back(4000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(250000);
        // bandwidthsList.push_back(250000);

        flog::info("starting ... constructor RadioModule");

        // 2,5; 5; 10; 12,5; 20; 25; 30 или 50 кГц.
        snapintervalsList.clear();
        if (clnt_mode == 1)
        {
            snapintervalsListTxt = snapintervalsListTxtAzlClnt;
            snapintervalsList.push_back(1);
            snapintervalsList.push_back(2);
            snapintervalsList.push_back(3);
            snapintervalsList.push_back(4);
            snapintervalsList.push_back(5);
            snapintervalsList.push_back(10);
            snapintervalsList.push_back(20);
            snapintervalsList.push_back(30);
            snapintervalsList.push_back(40);
            snapintervalsList.push_back(50);
            snapintervalsList.push_back(100);
        }
        else
        {
            snapintervalsList.push_back(1000);
            snapintervalsList.push_back(2500);
            snapintervalsList.push_back(5000);
            snapintervalsList.push_back(6250);
            snapintervalsList.push_back(10000);
            snapintervalsList.push_back(12500);
            snapintervalsList.push_back(20000);
            snapintervalsList.push_back(25000);
            snapintervalsList.push_back(30000);
            snapintervalsList.push_back(50000);
            snapintervalsList.push_back(100000);
        }

        flog::info("starting 0 ... constructor RadioModule");
        // Register the menu
        gui::menu.registerEntry(name, menuHandler, this, this);

        // Register the module interface
        core::modComManager.registerInterface("radio", name, moduleInterfaceHandler, this);

        /*
        if(selectedDemodID==RADIO_DEMOD_USB || selectedDemodID==RADIO_DEMOD_LSB ) {
            //LSB, USB
            if(bandwidth>6250)
                bandwidth = 2700;
        }
        */
        bandwidthIQ = 50000;
        bandwidthId = 0;
        for (int i = 0; i < bandwidthsList.size(); i++)
        {
            // flog::info("TRACE. bandwidthsList[i] = {0}!", bandwidthsList[i]);
            if (bandwidthsList[i] >= bandwidth)
            {
                bandwidthId = i;
                flog::info("TRACE. bandwidth 4 = {0}, i= {1} !", bandwidth, i);
                break;
            }
        }

        // flog::info("TRACE. bandwidth 5 = {0}!", bandwidth);

        snapIntervalId = 0;
        for (int i = 0; i < snapintervalsList.size(); i++)
        {
            // flog::info("TRACE. snapintervalsList[i] = {0}, snapInterval = {1}!", snapintervalsList[i], snapInterval);
            if (snapintervalsList[i] >= snapInterval)
            {
                snapIntervalId = i;
                flog::info("TRACE. snapInterval = {0}!", snapInterval);
                break;
            }
        }
        // std::string currSource =  sourcemenu::getCurrSource();
        if (this->name == "Канал приймання")
        {
            core::configManager.acquire();
            if (core::configManager.conf["IsARM"] == true)
                isARM = true;
            else
                isARM = false;

            if (core::configManager.conf["IsServer"] == true)
                isServer = true;
            else
                isServer = false;
            core::configManager.release();
        }
        else
        {
            isServer = false;
            isARM = false;
        }

        // if (currSource != "ARM") {
        // else
        //    gui::mainWindow.setUpdateMenuSnd2Radio(false);
        // flog::info("TRACE. bandwidth 6 = {0}!", bandwidth);
        flog::info("RadioModule OK");
    }

    ~RadioModule()
    {
        flog::info(" ~RadioModule()");
        pleaseStop.store(true);
        if (workerThread.joinable())
        {
            flog::info("workerThread.detach()");
            workerThread.join();
        }
        /*
        // Безопасно удаляем последний активный демодулятор
        if (selectedDemod)
        {
            delete selectedDemod;
            selectedDemod = NULL;
        }
        */
        // И удаляем тот, что остался в "мусорном ведре"
        if (demodToKill)
        {
            delete demodToKill;
            demodToKill = NULL;
        }

        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled)
        {
            disable();
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit()
    {

        // 1. Создаем VFO (теперь это безопасно, т.к. iqFrontEnd уже инициализирован)
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);

        // 2. Назначаем обработчики
        onUserChangedBandwidthHandler.handler = vfoUserChangedBandwidthHandler;
        onUserChangedBandwidthHandler.ctx = this;
        vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);

        // 3. Строим DSP-цепочки
        // Initialize IF DSP chain
        ifChainOutputChanged.ctx = this;
        ifChainOutputChanged.handler = ifChainOutputChangeHandler;
        ifChain.init(vfo->output);

        nb.init(NULL, 500.0 / 24000.0, 10.0);
        fmnr.init(NULL, 32);
        squelch.init(NULL, MIN_SQUELCH);

        ifChain.addBlock(&nb, false);
        ifChain.addBlock(&squelch, false);
        ifChain.addBlock(&fmnr, false);

        // Initialize audio DSP chain
        afChain.init(&dummyAudioStream);

        resamp.init(NULL, 250000.0, 48000.0);
        deemp.init(NULL, 50e-6, 48000.0);

        afChain.addBlock(&resamp, true);
        afChain.addBlock(&deemp, false);

        // 4. Инициализируем Sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(afChain.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);
        flog::info("registerStream2");

        // 5. Выбираем демодулятор (этот вызов теперь тоже безопасен)
        flog::info("Start selectDemodByID in postInit");
        selectDemodByID((DemodID)selectedDemodID);
        flog::info("selectDemodByID in postInit");

        // 6. Запускаем цепочки
        ifChain.start();
        afChain.start();
        stream.start();

        currSource = sourcemenu::getCurrSource(); //  sigpath::sourceManager::getCurrSource();

        if (currSource == SOURCE_ARM)
            isARM = true;
        else
            isARM = false;

        if (name == "Канал приймання")
        {
            if (isServer)
            {
                flog::info("TRACE SND menuHandler 2 update_menu {0}, selectedDemodID {1}, bandwidth {2}, isARM {3}, isServer {4}", update_menu, selectedDemodID, bandwidth, isARM, isServer);
                gui::mainWindow.setselectedDemodID(selectedDemodID);
                gui::mainWindow.setbandwidth(bandwidth);
                gui::mainWindow.setsnapInterval(snapInterval);
                gui::mainWindow.setsnapIntervalId(snapIntervalId);
                gui::mainWindow.setdeempId(deempId);
                gui::mainWindow.setnbEnabled(nbEnabled);
                gui::mainWindow.setnbLevel(nbLevel);
                gui::mainWindow.setsquelchEnabled(squelchEnabled);
                gui::mainWindow.setsquelchLevel(squelchLevel);
                gui::mainWindow.setFMIFNREnabled(FMIFNREnabled);
                gui::mainWindow.setfmIFPresetId(fmIFPresetId);
                gui::mainWindow.setUpdateMenuSnd2Radio(true);
            }
            else
                gui::mainWindow.setUpdateMenuSnd2Radio(false);
        }
        flog::info("postInit RADIO {0}", this->name);
        if (this->name == "Канал приймання" && (isARM || isServer))
            workerThread = std::thread(&RadioModule::workerInfo, this);
    }

    void enable()
    {
        enabled = true;
        if (!vfo)
        {
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 1000, 200000, false);
            vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);
            vfo->wtfVFO->onUserChangedDemodulator.bindHandler(&onUserChangedDemodulatorHandler);
        }
        ifChain.setInput(vfo->output, [=](dsp::stream<dsp::complex_t> *out)
                         { ifChainOutputChangeHandler(out, this); });
        ifChain.start();
        selectDemodByID((DemodID)selectedDemodID);
        afChain.start();
    }

    void disable()
    {
        enabled = false;
        ifChain.stop();
        if (selectedDemod)
        {
            selectedDemod->stop();
        }
        afChain.stop();
        if (vfo)
        {
            sigpath::vfoManager.deleteVFO(vfo);
        }
        vfo = NULL;
    }

    bool isEnabled()
    {
        return enabled;
    }

    std::string name;

    enum DemodID
    {
        RADIO_DEMOD_NFM,
        RADIO_DEMOD_WFM,
        RADIO_DEMOD_AM,
        RADIO_DEMOD_DSB,
        RADIO_DEMOD_USB,
        RADIO_DEMOD_CW,
        RADIO_DEMOD_LSB,
        RADIO_DEMOD_RAW,
        RADIO_DEMOD_IQ
    };

//// NFM - ЧМ; WFM - ЧМ-Ш; AM -АМ; DSB - ПБС; LSB - НБС; USB - ВБС; CW - НС; RAW - CMO
#define strNFM "ЧМ"
#define strWFM "ЧМ-Ш"
#define strAM "AM"
#define strDSB "ПБС"
#define strLSB "НБС"
#define strUSB "ВБС"
#define strCW "HC"
#define strRAW "CMO"
    // #define strIQ "IQ"

private:
    static void workerInfo(void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;
        flog::info("[workerInfo] RadioModule");

        bool DEBUG = false;
        // Receive loop
        while (!_this->pleaseStop.load()) // true
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                if (_this->name != "Канал приймання")
                {
                    continue;
                }

                // flog::info("TRACE RCV RADIO menuHandler, name {0}", _this->name);
                if (gui::mainWindow.getUpdateMenuRcv2Radio())
                {
                    std::lock_guard<std::mutex> lck(_this->Mtx);
                    if (_this->isARM && gui::mainWindow.getServerStatus(gui::mainWindow.getCurrServer()) != ARM_STATUS_FULL_CONTROL)
                    {
                        continue;
                    }
                    flog::info("TRACE RCV RADIO menuHandler 1 update_menu {0}, getlinearGain() {1}, getselectedDemodID() {2}, _this->snapIntervalId {3}, gui::mainWindow.getsnapIntervalId {4}", _this->update_menu, _this->selectedDemodID, gui::mainWindow.getselectedDemodID(), _this->snapIntervalId, gui::mainWindow.getsnapIntervalId());
                    int newDemodId = gui::mainWindow.getselectedDemodID();
                    if (newDemodId != _this->selectedDemodID)
                    {
                        flog::info("WORKER: Requesting demod change to {0}", newDemodId);
                        _this->requestedDemodId.store(newDemodId); // Просто кладем записку
                    }
                    _this->selectedDemodID = newDemodId; // gui::mainWindow.getselectedDemodID();
                    // _this->snapInterval = gui::mainWindow.getsnapInterval();
                    // _this->snapIntervalId = gui::mainWindow.getsnapIntervalId();
                    // flog::info("TRACE RCV RADIO _this->snapIntervalId {0}", _this->snapIntervalId);

                    if (DEBUG)
                        flog::info("[workerInfo] Start selectDemodByID in workerInfo/RadioModule");
                    if (_this->selectedDemodID == 0)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_NFM);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 1)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_WFM);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 2)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_AM);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 3)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_DSB);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 4)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_USB);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 5)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_CW);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 6)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_LSB);
                        int recMode = 1;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 7)
                    {
                        _this->selectDemodByID(RADIO_DEMOD_RAW);
                        int recMode = 0;
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }
                    else if (_this->selectedDemodID == 8)
                    {
                        int recMode = 0;
                        if (IQ)
                        {
                            _this->selectDemodByID(RADIO_DEMOD_IQ);
                            recMode = 2; // RECORDER_MODE_PUREIQ
                        }
                        else
                        {
                            _this->selectDemodByID(RADIO_DEMOD_RAW);
                        }
                        core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                    }

                    if (DEBUG)
                        flog::info("[workerInfo] Finish selectDemodByID in workerInfo/RadioModule");

                    if (std::isnan(gui::mainWindow.getbandwidth()))
                    {
                        flog::error("WORKER: Received NaN for bandwidth! Ignoring update.");
                        gui::mainWindow.setUpdateMenuRcv2Radio(false); // Сбрасываем флаг, чтобы не зациклиться
                        continue;                                      // Пропускаем всю остальную логику
                    }
                    else
                    {
                        flog::info("[workerInfo] Start setBandwidth in workerInfo/RadioModule  Закоментированно. gui::mainWindow.getbandwidth() {0}, _this->bandwidth {1}", gui::mainWindow.getbandwidth(), _this->bandwidth);
                        if (_this->bandwidth != gui::mainWindow.getbandwidth())
                        {
                            _this->bandwidth = gui::mainWindow.getbandwidth();
                            _this->setBandwidth(_this->bandwidth);
                        }
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish setBandwidth in workerInfo/RadioModule");
                    if (DEBUG)
                        flog::info("[workerInfo] Start setBandwidth in workerInfo/RadioModule");
                    if (_this->snapIntervalId != gui::mainWindow.getsnapIntervalId())
                    {
                        flog::info("TRACE RCV RADIO 3. _this->snapIntervalId {0}, gui::mainWindow.getsnapIntervalId() {1}, gui::mainWindow.getsnapIntervalId() {2}", _this->snapIntervalId, gui::mainWindow.getsnapIntervalId(), gui::mainWindow.getsnapIntervalId());
                        _this->snapIntervalId = gui::mainWindow.getsnapIntervalId();
                        int snap = gui::mainWindow.getsnapInterval();
                        if (snap != _this->snapInterval)
                        {
                            _this->snapInterval = gui::mainWindow.getsnapInterval();
                            if (_this->snapInterval < 1)
                            {
                                _this->snapInterval = 1;
                            }
                            _this->vfo->setSnapInterval(_this->snapInterval);
                            config.acquire();
                            config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
                            config.release(true);
                        }
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish setBandwidth in workerInfo/RadioModule");

                    if (DEBUG)
                        flog::info("[workerInfo] Start nbAllowed in workerInfo/RadioModule");
                    if (_this->nbAllowed)
                    {
                        if (_this->nbEnabled != gui::mainWindow.getnbEnabled())
                        {
                            _this->nbEnabled = gui::mainWindow.getnbEnabled();
                            _this->setNBEnabled(_this->nbEnabled);
                        }
                        if (_this->nbLevel != gui::mainWindow.getnbLevel())
                        {
                            _this->nbLevel = gui::mainWindow.getnbLevel();
                            _this->setNBLevel(_this->nbLevel);
                        }
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish baseband_band in workerInfo/RadioModule");

                    if (_this->selectedDemodID == RADIO_DEMOD_RAW)
                    {
                        if (_this->baseband_band != gui::mainWindow.getCMO_BBand())
                        {
                            _this->baseband_band = gui::mainWindow.getCMO_BBand();
                            _this->setCMO_BBand(_this->baseband_band);
                        }
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish baseband_band in workerInfo/RadioModule");

                    if (DEBUG)
                        flog::info("[workerInfo] Start setSquelchEnabled in workerInfo/RadioModule");
                    if (_this->squelchEnabled != gui::mainWindow.getsquelchEnabled())
                    {
                        _this->squelchEnabled = gui::mainWindow.getsquelchEnabled();
                        _this->setSquelchEnabled(_this->squelchEnabled);
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish setSquelchEnabled in workerInfo/RadioModule");

                    if (DEBUG)
                        flog::info("[workerInfo] Start setSquelchLevel in workerInfo/RadioModule");
                    if (_this->squelchLevel != gui::mainWindow.getsquelchLevel())
                    {
                        _this->squelchLevel = gui::mainWindow.getsquelchLevel();
                        _this->setSquelchLevel(_this->squelchLevel);
                    }
                    if (DEBUG)
                        flog::info("[workerInfo] Finish setSquelchLevel in workerInfo/RadioModule");
                    if (_this->FMIFNRAllowed)
                    { /*
                      if (_this->FMIFNREnabled != gui::mainWindow.getFMIFNREnabled())
                      {
                          _this->FMIFNREnabled = gui::mainWindow.getFMIFNREnabled();
                          _this->setFMIFNREnabled(_this->FMIFNREnabled);
                      }
                      if (_this->fmIFPresetId != gui::mainWindow.getfmIFPresetId())
                      {
                          _this->fmIFPresetId = gui::mainWindow.getfmIFPresetId();
                          _this->setIFNRPreset(_this->ifnrPresets[_this->fmIFPresetId]);
                      }
                      */
                    }
                    gui::mainWindow.setUpdateMenuRcv2Radio(false);
                    gui::mainWindow.setUpdateMODRadio(true);
                }
            }
        }
    }

    int findNearestValue(int oldVal, int newVal)
    {
        std::vector<int> values = {200, 400, 600, 750, 1000, 1500, 2000, 3000, 5000};
        int nearest = values[0];

        if (newVal > oldVal)
        {
            for (int num : values)
            {
                if (num >= newVal)
                {
                    nearest = num;
                    break;
                }
            }
        }
        else if (newVal < oldVal)
        {
            for (int i = values.size() - 1; i >= 0; --i)
            {
                if (values[i] <= newVal)
                {
                    nearest = values[i];
                    break;
                }
            }
        }
        else
        {
            int minDiff = std::abs(newVal - values[0]);
            for (int num : values)
            {
                int diff = std::abs(newVal - num);
                if (diff < minDiff)
                {
                    nearest = num;
                    minDiff = diff;
                }
            }
        }
        if (nearest == 200)
            nearest = 280;
        return nearest;
    }

    static void menuHandler(void *ctx)
    {
        if (gui::mainWindow.getStopMenuUI())
        {
            return;
        }
        RadioModule *_this = (RadioModule *)ctx;
        std::lock_guard<std::mutex> lck(_this->Mtx);
        _this->update_menu = false;
        int _work = 0;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
        bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();

        /*
        int demodToSet = _this->requestedDemodId.exchange(-1); // Читаем и сбрасываем
        if (demodToSet != -1)
        {
            flog::info("MENU HANDLER: Applying requested demod change to {0}", demodToSet);
            _this->selectDemodByID((DemodID)demodToSet);
            //
        }
        */
        if (!_this->enabled || _work > 0 || _ifStartElseBtn)
        {
            ImGui::BeginDisabled();
        }
        float menuWidth = ImGui::GetContentRegionAvail().x;
        // flog::info("_work {0}, _ifStartElseBtn {1}", _work, _ifStartElseBtn);
        ImGui::BeginGroup();
        ImGui::Columns(4, CONCAT("RadioModeColumns##_", _this->name), false);
        std::string _mode = CONCAT(strNFM, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 0) && _this->selectedDemodID != 0)
        {
            _this->selectDemodByID(RADIO_DEMOD_NFM);
            // _this->requestedDemodId.store(RADIO_DEMOD_NFM);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        _mode = CONCAT(strWFM, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 1) && _this->selectedDemodID != 1)
        {
            _this->selectDemodByID(RADIO_DEMOD_WFM);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        ImGui::NextColumn();
        _mode = CONCAT(strAM, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 2) && _this->selectedDemodID != 2)
        {
            _this->selectDemodByID(RADIO_DEMOD_AM);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        _mode = CONCAT(strDSB, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 3) && _this->selectedDemodID != 3)
        {
            _this->selectDemodByID(RADIO_DEMOD_DSB);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        ImGui::NextColumn();
        _mode = CONCAT(strUSB, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 4) && _this->selectedDemodID != 4)
        {
            _this->selectDemodByID(RADIO_DEMOD_USB);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        _mode = CONCAT(strCW, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 5) && _this->selectedDemodID != 5)
        {
            _this->selectDemodByID(RADIO_DEMOD_CW);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        };
        ImGui::NextColumn();
        _mode = CONCAT(strLSB, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 6) && _this->selectedDemodID != 6)
        {
            _this->selectDemodByID(RADIO_DEMOD_LSB);
            int recMode = 1;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        }
        _mode = CONCAT(strRAW, "##_");
        if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 7) && _this->selectedDemodID != 7)
        {
            _this->selectDemodByID(RADIO_DEMOD_RAW);
            int recMode = 0;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
            _this->update_menu = true;
        };
        /*
        if (IQ)
        {
            _mode = CONCAT(strIQ, "##_");
            if (ImGui::RadioButton(CONCAT(_mode, _this->name), _this->selectedDemodID == 8) && _this->selectedDemodID != 8)
            {
                _this->selectDemodByID(RADIO_DEMOD_IQ);
                int recMode = 2; // RECORDER_MODE_PUREIQ
                flog::info("   IQ  recMode={0}", recMode);
                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                // _this->update_menu = true;
            };
        }
        */

        ImGui::Columns(1, CONCAT("EndRadioModeColumns##_", _this->name), false);

        ImGui::EndGroup();

        float inputsize = menuWidth / 2 - ImGui::GetCursorPosX();

        if (!_this->bandwidthLocked)
        {
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            /*
            if (_this->selectedDemodID == RADIO_DEMOD_IQ)
            {
            }
            else if (_this->radioMode == MODE_AZALIY)
            { //  || _this->selectedDemodID == RECORDER_MODE_PUREIQ
                ImGui::LeftLabel("Смуга, Гц");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::InputFloat(("##_radio_bw1_" + _this->name).c_str(), &_this->bandwidth, 100, 1000, "%.0f"))
                {
                    _this->setBandwidth(_this->bandwidth);
                    // _this->update_menu = true;
                }
            }
            else
            {
            */
            ImGui::LeftLabel("Смуга, кГц  ");
            inputsize = menuWidth / 2 - ImGui::GetCursorPosX();
            if (inputsize < menuWidth / 4)
                inputsize = menuWidth / 4;

            ImGui::SetNextItemWidth(inputsize);
            float kHzbandwidth = 1.0;
            if (_this->bandwidth < 1000)
                kHzbandwidth = 1.0;
            else
                kHzbandwidth = _this->bandwidth / 1000.0;

            if (ImGui::InputFloat(("##_radio_bw2_" + _this->name).c_str(), &kHzbandwidth, 1.25, 10.0, "%.2f"))
            {
                int max_bw = _this->bandwidthsList[_this->bandwidthsList.size() - 1];
                if (max_bw > _this->maxBandwidth)
                    max_bw = _this->maxBandwidth;
                float bw = std::clamp<double>(kHzbandwidth * 1000, 1, max_bw);
                if (bw >= _this->bandwidth)
                {
                    for (int i = 1; i < _this->bandwidthsList.size(); i++)
                    {
                        if (bw < _this->bandwidthsList[i])
                        {
                            bw = _this->bandwidthsList[i];
                            break;
                        }
                    }
                }
                else
                {
                    for (int i = _this->bandwidthsList.size() - 1; i >= 0; i--)
                    {
                        if (bw > _this->bandwidthsList[i])
                        {
                            bw = _this->bandwidthsList[i];
                            break;
                        }
                    }
                }
                if (bw < 1000)
                {
                    bw = 1000;
                }

                // flog::info("bw {0}", bw);
                flog::info("kHzbandwidth {0}, _this->bandwidth {1}, max_bw {2}, maxBandwidth {3}, bw {4}", kHzbandwidth, _this->bandwidth, max_bw, _this->maxBandwidth, bw);
                // _this->bandwidth = kHzbandwidth * 1000;
                _this->setBandwidth(bw);
                _this->update_menu = true;
            }
            // }
            // VFO snap interval
            ImGui::SameLine();
            ImGui::LeftLabel("Крок, кГц  ");
            ImGui::SetNextItemWidth(inputsize);

            if (_this->clnt_mode == 1)
            {
                if (ImGui::Combo(("##_radio_snap_" + _this->name).c_str(), &_this->snapIntervalId, _this->snapintervalsListTxt))
                {
                    _this->snapInterval = _this->snapintervalsList[_this->snapIntervalId];
                    if (_this->snapInterval < 1)
                    {
                        _this->snapInterval = 1;
                    }
                    _this->vfo->setSnapInterval(_this->snapInterval);
                    config.acquire();
                    config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
                    config.release(true);
                    _this->update_menu = true;
                }
            }
            else
            {
                float _snap = 1.0;
                if (_this->snapInterval < 1000)
                    _snap = 1.0;
                else
                    _snap = _this->snapInterval / 1000.0;
                // flog::info("_snap {0}, _this->snapInterval  {1}", _snap, _this->snapInterval);
                if (ImGui::InputFloat(("##_radio_snap_" + _this->name).c_str(), &_snap, 1.05, 10.0, "%.2f"))
                {
                    //_this->snapInterval = _this->snapintervalsList[_this->snapIntervalId];
                    float max_snap = _this->snapintervalsList[_this->snapintervalsList.size() - 1];
                    float snap = std::clamp<float>(_snap * 1000, 1, max_snap);
                    if (snap > max_snap)
                        snap = max_snap;
                    if (snap >= _this->snapInterval)
                    {
                        for (int i = 1; i < _this->snapintervalsList.size(); i++)
                        {
                            if (snap < _this->snapintervalsList[i])
                            {
                                snap = _this->snapintervalsList[i];
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (int i = _this->snapintervalsList.size() - 1; i >= 0; i--)
                        {
                            if (snap > _this->snapintervalsList[i])
                            {
                                snap = _this->snapintervalsList[i];
                                break;
                            }
                        }
                    }
                    if (snap < 1000)
                    {
                        snap = 1000;
                    }
                    flog::info("_snap {0}, max_snap {1}, _this->snapInterval {2}, snap_new {3}", _snap, max_snap, _this->snapInterval, snap);
                    _this->snapInterval = snap;
                    for (int i = 0; i < _this->snapintervalsList.size(); i++)
                    {
                        if (_this->snapintervalsList[i] >= _this->snapInterval)
                        {
                            _this->snapIntervalId = i;
                            break;
                        }
                    }
                    _this->vfo->setSnapInterval(_this->snapInterval);
                    config.acquire();
                    config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
                    config.release(true);
                    _this->update_menu = true;
                }
            }
        }
        if (_this->Admin)
        {

            // Deemphasis mode
            if (_this->deempAllowed)
            {
                ImGui::LeftLabel("Послаблення ВЧ");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo(("##_radio_wfm_deemp_" + _this->name).c_str(), &_this->deempId, _this->deempModes.txt))
                {
                    _this->setDeemphasisMode(_this->deempModes[_this->deempId]);
                    _this->update_menu = true;
                }
            }

            // Noise blanker
            if (_this->selectedDemodID < 7)
            {
                if (_this->nbAllowed)
                {
                    if (ImGui::Checkbox(("Шумоподавлювач##_radio_nb_ena_" + _this->name).c_str(), &_this->nbEnabled))
                    {
                        _this->setNBEnabled(_this->nbEnabled);
                        _this->update_menu = true;
                    }
                    if (!_this->nbEnabled && _this->enabled)
                    {
                        style::beginDisabled();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                    if (ImGui::SliderFloat(("##_radio_nb_lvl_" + _this->name).c_str(), &_this->nbLevel, _this->MIN_NB, _this->MAX_NB, "%.3fдБ"))
                    {
                        _this->setNBLevel(_this->nbLevel);
                        _this->update_menu = true;
                    }
                    if (!_this->nbEnabled && _this->enabled)
                    {
                        style::endDisabled();
                    }
                }
            }
        }
        else
        {
            _this->deempId = DEEMP_MODE_NONE;
            _this->nbEnabled = false;
        }
        /*
        if (_this->selectedDemodID == 7) {
            ImGui::LeftLabel("Смуга для СМО, кГц    ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            // int bb = _this->baseband_band / 1000000;

            int new_bb = _this->baseband_band / 1000;
            int step_val = 200;
            if (new_bb < 2000)
                step_val = 200;
            else
                step_val = 1000;
            int old_bb = new_bb;
            if (ImGui::SliderInt(("##_radio_raw_band_" + _this->name).c_str(), &new_bb, step_val, 5000)) { // MIN_BBand/1000, MAX_BBand/1000)) {
                if (new_bb != _this->curr_bb) {
                    int bb = _this->findNearestValue(old_bb, new_bb);
                    // flog::info("new_bb {0}, old_bb {1} = bb {2}, curr_bb {3} ", new_bb, old_bb, bb, _this->curr_bb);
                    _this->curr_bb = new_bb;
                    new_bb = bb;
                    _this->baseband_band = bb * 1000;
                    // if (bb == 8)
                    //    _this->baseband_band = _this->baseband_band + 500000;
                    _this->setCMO_BBand(_this->baseband_band);
                    gui::mainWindow.setCMO_BBand(_this->baseband_band);
                    //_this->update_menu = true;
                    flog::info("baseband_band {0}", _this->baseband_band);
                }
            }
        }
        */
        if (_this->selectedDemodID == RADIO_DEMOD_IQ)
        {
            ImGui::LeftLabel("Смуга, Гц");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputFloat(("##_radio_iq_bw_" + _this->name).c_str(), &_this->bandwidthIQ, 500, 1000, "%.0f"))
            {
                _this->setBandwidthIQ(_this->bandwidthIQ);
                // _this->update_menu = true;
            }
        }
        // Squelch
        _this->squelchEnabled = false;
        /*
        if (ImGui::Checkbox(("Рівень ШП##_radio_sqelch_ena_" + _this->name).c_str(), &_this->squelchEnabled)) {
            _this->setSquelchEnabled(_this->squelchEnabled);
            _this->update_menu = true;
        }
        if (!_this->squelchEnabled && _this->enabled) { style::beginDisabled(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##_radio_sqelch_lvl_" + _this->name).c_str(), &_this->squelchLevel, _this->MIN_SQUELCH, _this->MAX_SQUELCH, "%.3fдБ")) {
            _this->setSquelchLevel(_this->squelchLevel);
            _this->update_menu = true;
        }
        if (!_this->squelchEnabled && _this->enabled) { style::endDisabled(); }
        */
        if (_this->Admin)
        {
            // FM IF Noise Reduction

            if (_this->FMIFNRAllowed)
            {
                if (ImGui::Checkbox(("Зменшення шуму ПЧ##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->FMIFNREnabled))
                {
                    _this->setFMIFNREnabled(_this->FMIFNREnabled);
                    _this->update_menu = true;
                }
                if (_this->selectedDemodID == RADIO_DEMOD_NFM)
                {
                    if (!_this->FMIFNREnabled && _this->enabled)
                    {
                        style::beginDisabled();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                    if (ImGui::Combo(("##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->fmIFPresetId, _this->ifnrPresets.txt))
                    {
                        _this->setIFNRPreset(_this->ifnrPresets[_this->fmIFPresetId]);
                        _this->update_menu = true;
                    }
                    if (!_this->FMIFNREnabled && _this->enabled)
                    {
                        style::endDisabled();
                    }
                }
            }
        }
        else
        {
            _this->FMIFNREnabled = false;
        }

        if (_this->update_menu == true)
        {
            flog::info("TRACE SND RADIO menuHandler 2 _this->update_menu {0}, _this->selectedDemodID {1}, _this->bandwidth {2}", _this->update_menu, _this->selectedDemodID, _this->bandwidth);
            gui::mainWindow.setselectedDemodID(_this->selectedDemodID);
            gui::mainWindow.setbandwidth(_this->bandwidth);
            gui::mainWindow.setsnapInterval(_this->snapInterval);
            gui::mainWindow.setsnapIntervalId(_this->snapIntervalId);
            gui::mainWindow.setdeempId(_this->deempId);
            gui::mainWindow.setnbEnabled(_this->nbEnabled);
            gui::mainWindow.setnbLevel(_this->nbLevel);
            gui::mainWindow.setCMO_BBand(_this->baseband_band);
            gui::mainWindow.setsquelchEnabled(_this->squelchEnabled);
            gui::mainWindow.setsquelchLevel(_this->squelchLevel);
            gui::mainWindow.setFMIFNREnabled(_this->FMIFNREnabled);
            gui::mainWindow.setfmIFPresetId(_this->fmIFPresetId);
            gui::mainWindow.setUpdateMenuSnd2Radio(true); //_this->update_menu
            _this->update_menu = false;
        }

        // Demodulator specific menu
        _this->selectedDemod->showMenu();

        if (!_this->enabled || _work > 0 || _ifStartElseBtn)
        {
            ImGui::EndDisabled();
        }
    }

    demod::Demodulator *instantiateDemod(DemodID id)
    {
        demod::Demodulator *demod = NULL;
        switch (id)
        {
        case DemodID::RADIO_DEMOD_NFM:
            demod = new demod::NFM();
            break;
        case DemodID::RADIO_DEMOD_WFM:
            demod = new demod::WFM();
            break;
        case DemodID::RADIO_DEMOD_AM:
            demod = new demod::AM();
            break;
        case DemodID::RADIO_DEMOD_DSB:
            demod = new demod::DSB();
            break;
        case DemodID::RADIO_DEMOD_USB:
            demod = new demod::USB();
            break;
        case DemodID::RADIO_DEMOD_CW:
            demod = new demod::CW();
            break;
        case DemodID::RADIO_DEMOD_LSB:
            demod = new demod::LSB();
            break;
        case DemodID::RADIO_DEMOD_RAW:
            demod = new demod::RAW();
            break;
        case DemodID::RADIO_DEMOD_IQ:
            demod = new demod::pureIQ();
            break;
        default:
            demod = NULL;
            break;
        }
        if (!demod)
        {
            return NULL;
        }

        // Default config
        double bw = demod->getDefaultBandwidth();

        if (!config.conf[name].contains(demod->getName()))
        {
            config.acquire();
            config.conf[name][demod->getName()]["bandwidth"] = bw;
            config.conf[name][demod->getName()]["snapInterval"] = demod->getDefaultSnapInterval();
            config.conf[name][demod->getName()]["squelchLevel"] = MIN_SQUELCH;
            config.conf[name][demod->getName()]["squelchEnabled"] = false;
            config.release(true);
            // bandwidthId = 0;
            for (int i = 0; i < bandwidthsList.size(); i++)
            {
                if (bandwidthsList[i] == bandwidth)
                {
                    // bandwidthId = i;
                    flog::info("TRACE. bandwidth 2 = {0} bandwidthId = {1}!", bandwidth, bandwidthId);
                    break;
                }
            }

            snapIntervalId = 0;
            for (int i = 0; i < snapintervalsList.size(); i++)
            {
                // flog::info("TRACE. snapintervalsList[i] = {0}!", snapintervalsList[i]);

                if (snapintervalsList[i] == snapInterval)
                {
                    snapIntervalId = i;
                    flog::info("TRACE. snapInterval = {0}!", snapInterval);
                    break;
                }
            }
            config.release(true);
        }

        bw = std::clamp<double>(bw, demod->getMinBandwidth(), demod->getMaxBandwidth());

        // Initialize
        if (id == DemodID::RADIO_DEMOD_IQ)
            demod->init(name, &config, ifChain.out, bandwidthIQ, stream.getSampleRate());
        else
            demod->init(name, &config, ifChain.out, bw, stream.getSampleRate());

        flog::info("stream.getSampleRate() = {}", stream.getSampleRate());
        // flog::info("ifChain.out SR = {}", selectedDemod->getIFSampleRate());

        return demod;
    }
    void selectDemodByID(DemodID id)
    {
        // ==========================================================
        std::stringstream ss;
        ss << std::this_thread::get_id();
        // flog::error("!!!!!!!!!! SELECT_DEMOD_BY_ID CALLED for demod {0} from THREAD ID: {1} !!!!!!!!!!", (int)id, ss.str());
        // ==========================================================

        auto startTime = std::chrono::high_resolution_clock::now();
        demod::Demodulator *demod = instantiateDemod(id);
        if (!demod)
        {
            flog::error("Demodulator {0} not implemented", (int)id);
            return;
        }
        selectedDemodID = id;
        flog::info("selectedDemodID {0}", selectedDemodID);
        selectDemod(demod);
        flog::info("selectDemod");
        // Save config
        config.acquire();
        config.conf[name]["selectedDemodId"] = id;
        config.release(true);
        auto endTime = std::chrono::high_resolution_clock::now();
        flog::warn("selectDemodByID {0} Demod switch took {1} мкс", selectedDemodID, (int64_t)((std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime)).count()));
    }

    // radio_module.cpp

    void selectDemod(demod::Demodulator *demod)
    {
        bool DEBUG = false;
        if (DEBUG)
            flog::error("========= selectDemod START =========");
        // ==========================================================
        // ШАГ 0: Безопасное удаление объекта из предыдущего вызова
        // ==========================================================
        if (DEBUG)
            flog::info("selectDemod: Step 0.1 - Checking for demod to kill...");
        if (demodToKill)
        {
            if (DEBUG)
                flog::warn("selectDemod: Step 0.2 - Deleting old demod '{0}'...", demodToKill->getName());
            delete demodToKill;
            demodToKill = NULL;
            if (DEBUG)
                flog::warn("selectDemod: Step 0.3 - Deletion complete.");
        }

        // ==========================================================
        // ШАГ 1: Остановка текущей DSP-цепочки
        // ==========================================================
        if (DEBUG)
            flog::info("selectDemod: Step 1.1 - Stopping components...");
        if (selectedDemod)
        {
            selectedDemod->stop();
        }
        afChain.stop();
        stream.stop();
        if (DEBUG)
            flog::info("selectDemod: Step 1.2 - Components stopped.");

        if (DEBUG)
            flog::info("selectDemod: Step 1.3 - Detaching afChain input...");
        afChain.setInput(&dummyAudioStream, [=](dsp::stream<dsp::stereo_t> *out)
                         { stream.setInput(out); });
        if (DEBUG)
            flog::info("selectDemod: Step 1.4 - afChain input detached.");
        // ==========================================================
        // ШАГ 2: Замена объекта (без 'delete')
        // ==========================================================
        if (DEBUG)
            flog::info("selectDemod: Step 2.1 - Swapping demodulator pointer...");
        if (selectedDemod)
        {
            // НЕ УДАЛЯЕМ, а просто перемещаем указатель в "камеру смертников"
            demodToKill = selectedDemod;
        }
        selectedDemod = demod;
        if (DEBUG)
            flog::info("selectDemod: Step 2.2 - Pointer swapped. New demod is '{0}'.", selectedDemod->getName());
        // ==========================================================
        // ШАГ 3: Настройка и запуск новой цепочки
        // ==========================================================
        if (DEBUG)
            flog::info("selectDemod: Step 3.1 - Configuring new demodulator...");
        selectedDemod->AFSampRateChanged(audioSampleRate);
        selectedDemod->setInput(ifChain.out);
        afChain.setInput(selectedDemod->getOutput(), [=](dsp::stream<dsp::stereo_t> *out)
                         { stream.setInput(out); });

        // Load config
        bandwidth = selectedDemod->getDefaultBandwidth();
        minBandwidth = selectedDemod->getMinBandwidth();
        maxBandwidth = selectedDemod->getMaxBandwidth();
        bandwidthLocked = selectedDemod->getBandwidthLocked();
        snapInterval = selectedDemod->getDefaultSnapInterval();
        squelchLevel = MIN_SQUELCH;
        deempAllowed = selectedDemod->getDeempAllowed();
        deempId = deempModes.valueId((DeemphasisMode)selectedDemod->getDefaultDeemphasisMode());
        squelchEnabled = false;
        postProcEnabled = selectedDemod->getPostProcEnabled();
        FMIFNRAllowed = selectedDemod->getFMIFNRAllowed();
        FMIFNREnabled = false;
        fmIFPresetId = ifnrPresets.valueId(IFNR_PRESET_VOICE);
        nbAllowed = selectedDemod->getNBAllowed();
        nbEnabled = false;
        nbLevel = 0.0f;
        double ifSamplerate = selectedDemod->getIFSampleRate();

        if (DEBUG)
            flog::info("selectDemod: Step 3.3 - Loading config and applying settings...");
        config.acquire();
        {
            // squelchLevel =-100;
            try
            {
                if (config.conf[name][selectedDemod->getName()].contains("bandwidth"))
                {
                    bandwidth = config.conf[name][selectedDemod->getName()]["bandwidth"];
                    if (selectedDemod->getName() == "RAW")
                    {
                        bandwidth = std::clamp<double>(bandwidth, 8000, maxBandwidth);
                    }
                    else
                    {
                        bandwidth = std::clamp<double>(bandwidth, 1000, maxBandwidth);
                    }
                }
                if (config.conf[name][selectedDemod->getName()].contains("snapInterval"))
                {
                    snapInterval = config.conf[name][selectedDemod->getName()]["snapInterval"];
                }
                if (config.conf[name][selectedDemod->getName()].contains("squelchLevel") && !config.conf[name][selectedDemod->getName()]["squelchLevel"].is_null())
                {
                    squelchLevel = config.conf[name][selectedDemod->getName()]["squelchLevel"];
                }
                if (config.conf[name][selectedDemod->getName()].contains("squelchEnabled"))
                {
                    squelchEnabled = config.conf[name][selectedDemod->getName()]["squelchEnabled"];
                }
                if (config.conf[name][selectedDemod->getName()].contains("deempMode"))
                {
                    if (!config.conf[name][selectedDemod->getName()]["deempMode"].is_string())
                    {
                        config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
                    }

                    std::string deempOpt = config.conf[name][selectedDemod->getName()]["deempMode"];
                    if (deempModes.keyExists(deempOpt))
                    {
                        deempId = deempModes.keyId(deempOpt);
                    }
                }
                if (config.conf[name][selectedDemod->getName()].contains("FMIFNREnabled"))
                {
                    FMIFNREnabled = config.conf[name][selectedDemod->getName()]["FMIFNREnabled"];
                }
                if (config.conf[name][selectedDemod->getName()].contains("fmifnrPreset"))
                {
                    std::string presetOpt = config.conf[name][selectedDemod->getName()]["fmifnrPreset"];
                    if (ifnrPresets.keyExists(presetOpt))
                    {
                        fmIFPresetId = ifnrPresets.keyId(presetOpt);
                    }
                }
                if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerEnabled"))
                {
                    nbEnabled = config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"];
                }
                if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerLevel"))
                {
                    nbLevel = config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"];
                }
                if (selectedDemodID == RADIO_DEMOD_RAW)
                {
                    try
                    {
                        baseband_band = config.conf[name][selectedDemod->getName()]["baseband_band"];
                        gui::mainWindow.setCMO_BBand(baseband_band);
                    }
                    catch (...)
                    {
                        baseband_band = baseband_band;
                    }
                }
                if (selectedDemodID == RADIO_DEMOD_IQ)
                {
                    try
                    {
                        bandwidthIQ = config.conf[name][selectedDemod->getName()]["bandwidthIQ"];
                        // gui::mainWindow.setCMO_BBand(baseband_band);
                    }
                    catch (...)
                    {
                        bandwidthIQ = 50000;
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
            }

            if (clnt_mode == 0 && selectedDemodID == RADIO_DEMOD_WFM)
            {
                if (bandwidth < 180000)
                {
                    bandwidth = 180000;
                    config.conf[name][selectedDemod->getName()]["bandwidth"] = bandwidth;
                }
                if (snapInterval != 100000)
                {
                    snapInterval = 100000;
                    config.conf[name][selectedDemod->getName()]["snapInterval"] = snapInterval;
                }
                config.conf[name][selectedDemod->getName()]["deempMode"] = DEEMP_MODE_NONE;
                if (squelchEnabled != false)
                {
                    squelchEnabled = false;
                    config.conf[name][selectedDemod->getName()]["squelchEnabled"] = squelchEnabled;
                }
                if (nbEnabled != false)
                {
                    nbEnabled = false;
                    config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"] = nbEnabled;
                }
                if (FMIFNREnabled != false)
                {
                    FMIFNREnabled = false;
                    config.conf[name][selectedDemod->getName()]["FMIFNREnabled"] = FMIFNREnabled;
                }
            }
            else if (clnt_mode == 1 && selectedDemodID == RADIO_DEMOD_USB)
            {
                if (bandwidth > 5000)
                {
                    bandwidth = 2700;
                    config.conf[name][selectedDemod->getName()]["bandwidth"] = bandwidth;
                }
                if (snapInterval != 100)
                {
                    snapInterval = 100;
                    config.conf[name][selectedDemod->getName()]["snapInterval"] = snapInterval;
                }
                if (squelchEnabled != false)
                {
                    squelchEnabled = false;
                    config.conf[name][selectedDemod->getName()]["squelchEnabled"] = squelchEnabled;
                }
                if (nbEnabled != false)
                {
                    nbEnabled = false;
                    config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"] = nbEnabled;
                }
                if (FMIFNREnabled != false)
                {
                    FMIFNREnabled = false;
                    config.conf[name][selectedDemod->getName()]["FMIFNREnabled"] = FMIFNREnabled;
                }
            }

            bandwidthId = 0;
            for (int i = 0; i < bandwidthsList.size(); i++)
            {
                if (bandwidthsList[i] >= bandwidth)
                {
                    bandwidthId = i;
                    flog::info("TRACE. bandwidth = {0}, i= {1} !", bandwidth, i);
                    break;
                }
            }
            // if(bandwidthId==0) bandwidthId = bandwidthsList.size();
        }
        config.release();
        if (DEBUG)
            flog::info("selectDemod: Step 3.4 - Settings applied.");

        if (vfo)
        {
            vfo->setReference(selectedDemod->getVFOReference());
            vfo->setSnapInterval(snapInterval);
            // flog::error("[ARM-DEBUG] Applying to VFO -> ifSamplerate: {0}, bandwidth: {1}", ifSamplerate, bandwidth);
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(ifSamplerate, bandwidth);
        }

        setBandwidth(bandwidth);
        nb.setRate(500.0 / ifSamplerate);
        setNBLevel(nbLevel);
        setNBEnabled(nbAllowed && nbEnabled);
        setIFNRPreset((selectedDemodID == RADIO_DEMOD_NFM) ? ifnrPresets[fmIFPresetId] : IFNR_PRESET_BROADCAST);
        setFMIFNREnabled(FMIFNRAllowed ? FMIFNREnabled : false);
        setSquelchLevel(squelchLevel);
        setSquelchEnabled(squelchEnabled);

        if (postProcEnabled)
        {
            resamp.setInSamplerate(selectedDemod->getAFSampleRate());
            setAudioSampleRate(audioSampleRate);
            afChain.enableBlock(&resamp, [=](dsp::stream<dsp::stereo_t> *out)
                                { stream.setInput(out); });
            if (!Admin)
                deempId = DEEMP_MODE_NONE;
            setDeemphasisMode(deempModes[deempId]);
        }
        else
        {
            afChain.disableAllBlocks([=](dsp::stream<dsp::stereo_t> *out)
                                     { stream.setInput(out); });
        }

        // ==========================================================
        // ШАГ 4: Финальный запуск
        // ==========================================================
        flog::info("Demod switch: Starting all components...");
        if (DEBUG)
            flog::info("selectDemod: Step 4.1 - Starting components...");
        selectedDemod->start();
        afChain.start();
        stream.start();
        if (DEBUG)
            flog::info("selectDemod: Step 4.2 - Components started.");

        flog::info("Demodulator switch complete.");
        if (DEBUG)
            flog::error("========= selectDemod FINISH =========");
    }

    void setBandwidthIQ(double bw)
    {
        int max_bw = bandwidthsList[bandwidthsList.size() - 1];
        if (max_bw > maxBandwidth)
            max_bw = maxBandwidth;
        bw = std::clamp<double>(bw, 1, max_bw);
        if (!selectedDemod)
        {
            return;
        }
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);
        config.acquire();
        config.conf[name][selectedDemod->getName()]["bandwidthIQ"] = bw;
        config.release(true);
    }

    void setBandwidth(double bw)
    {
        flog::info("setBandwidth {0}", bw);
        int max_bw = bandwidthsList[bandwidthsList.size() - 1];
        if (max_bw > maxBandwidth)
            max_bw = maxBandwidth;
        bw = std::clamp<double>(bw, 1, max_bw);
        if (selectedDemodID == RADIO_DEMOD_USB || selectedDemodID == RADIO_DEMOD_LSB)
        {
            if (bw >= 12500)
            {
                bw = 2700;
            }
        }

        if (radioMode == MODE_AZALIY || selectedDemodID == 8)
        {
            double tbw = ceil(bw / 100.0);
            // flog::info("tbw={0}, tbw*1000={1}", tbw, tbw*100);
            bandwidth = tbw * 100;
        }
        else
        {
            for (int i = 1; i < bandwidthsList.size() - 1; i++)
            {
                if (bw > bandwidthsList[i - 1] && bw < bandwidthsList[i])
                {
                    flog::info("TRACE!!! 2 bw old  = {0}, bw new = {1}, andwidthId = {1}!", bw, bandwidthsList[i], i);
                    bw = bandwidthsList[i];
                    break;
                }
            }
            bandwidth = bw;
        }
        if (bandwidth < 1000)
        {
            bandwidth = 1000;
        }
        // flog::info("bandwidth={0}", bandwidth);

        if (!selectedDemod)
        {
            return;
        }
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);

        flog::info("setBandwidth {0}, name {1}, selectedDemod->getName() {2}, bandwidth={3} ", bw, name, selectedDemod->getName(), bandwidth);

        config.acquire();
        config.conf[name][selectedDemod->getName()]["bandwidth"] = bw;
        config.release(true);
    }

    void setAudioSampleRate(double sr)
    {
        audioSampleRate = sr;
        if (!selectedDemod)
        {
            return;
        }
        selectedDemod->AFSampRateChanged(audioSampleRate);
        if (!postProcEnabled && vfo)
        {
            // If postproc is disabled, IF SR = AF SR
            minBandwidth = selectedDemod->getMinBandwidth();
            maxBandwidth = selectedDemod->getMaxBandwidth();
            bandwidth = selectedDemod->getIFSampleRate();
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
            return;
        }

        afChain.stop();

        // Configure resampler
        resamp.setOutSamplerate(audioSampleRate);

        // Configure deemphasis sample rate
        deemp.setSamplerate(audioSampleRate);

        afChain.start();
    }

    void setDeemphasisMode(DeemphasisMode mode)
    {
        if (!Admin)
            mode = DEEMP_MODE_NONE;
        deempId = deempModes.valueId(mode);
        if (!postProcEnabled || !selectedDemod)
        {
            return;
        }
        bool deempEnabled = (mode != DEEMP_MODE_NONE);
        if (deempEnabled)
        {
            deemp.setTau(deempTaus[mode]);
        }
        afChain.setBlockEnabled(&deemp, deempEnabled, [=](dsp::stream<dsp::stereo_t> *out)
                                { stream.setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
        config.release(true);
    }

    void setNBEnabled(bool enable)
    {
        nbEnabled = enable;
        if (!selectedDemod)
        {
            return;
        }
        ifChain.setBlockEnabled(&nb, nbEnabled, [=](dsp::stream<dsp::complex_t> *out)
                                { selectedDemod->setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"] = nbEnabled;
        config.release(true);
    }

    void setNBLevel(float level)
    {
        nbLevel = std::clamp<float>(level, MIN_NB, MAX_NB);
        nb.setLevel(nbLevel);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"] = nbLevel;
        config.release(true);
    }

    void setCMO_BBand(int val)
    {
        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["baseband_band"] = baseband_band;
        config.release(true);
    }

    void setSquelchEnabled(bool enable)
    {
        squelchEnabled = enable;
        if (!selectedDemod)
        {
            return;
        }
        ifChain.setBlockEnabled(&squelch, squelchEnabled, [=](dsp::stream<dsp::complex_t> *out)
                                { selectedDemod->setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchEnabled"] = squelchEnabled;
        config.release(true);
    }

    void setSquelchLevel(float level)
    {
        squelchLevel = std::clamp<float>(level, MIN_SQUELCH, MAX_SQUELCH);
        squelch.setLevel(squelchLevel);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchLevel"] = squelchLevel;
        config.release(true);
    }

    void setFMIFNREnabled(bool enabled)
    {
        FMIFNREnabled = enabled;
        if (!selectedDemod)
        {
            return;
        }
        ifChain.setBlockEnabled(&fmnr, FMIFNREnabled, [=](dsp::stream<dsp::complex_t> *out)
                                { selectedDemod->setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["FMIFNREnabled"] = FMIFNREnabled;
        config.release(true);
    }

    void setIFNRPreset(IFNRPreset preset)
    {
        if (!Admin)
            return;
        // Don't save if in broadcast mode
        if (preset == IFNR_PRESET_BROADCAST)
        {
            if (!selectedDemod)
            {
                return;
            }
            fmnr.setBins(ifnrTaps[preset]);
            return;
        }

        fmIFPresetId = ifnrPresets.valueId(preset);
        if (!selectedDemod)
        {
            return;
        }
        fmnr.setBins(ifnrTaps[preset]);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["fmifnrPreset"] = ifnrPresets.key(fmIFPresetId);
        config.release(true);
    }

    static void vfoUserChangedBandwidthHandler(double newBw, void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;
        _this->setBandwidth(newBw);
    }

    static void vfoUserChangedDemodulatorHandler(int newDemodulator, void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;
        if (_this->selectedDemodID != (DemodID)newDemodulator)
        {
            _this->selectDemodByID((DemodID)newDemodulator);
        }
    }

    static void sampleRateChangeHandler(float sampleRate, void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void ifChainOutputChangeHandler(dsp::stream<dsp::complex_t> *output, void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;
        if (!_this->selectedDemod)
        {
            return;
        }
        _this->selectedDemod->setInput(output);
    }

    static void moduleInterfaceHandler(int code, void *in, void *out, void *ctx)
    {
        RadioModule *_this = (RadioModule *)ctx;        
        if (!_this->enabled || !_this->selectedDemod)
        {
            return;
        }

        // Execute commands
        if (code == RADIO_IFACE_CMD_GET_MODE && out)
        {
            flog::info("RADIO_IFACE_CMD_GET_MODE  2");
            int *_out = (int *)out;
            *_out = _this->selectedDemodID;
        }
        else if (code == RADIO_IFACE_CMD_SET_MODE && in)
        {
            int *_in = (int *)in;
            DemodID _newdemodID = (DemodID)*_in;
            if (_this->selectedDemodID != _newdemodID)
            {
                float bw = _this->bandwidth;
                _this->selectDemodByID(_newdemodID);
                gui::mainWindow.setsnapInterval(_this->snapInterval);
                gui::mainWindow.setselectedDemodID(_this->selectedDemodID);
                if (bw != _this->bandwidth)
                {
                    flog::info("CMD_SET_BANDWIDTH 1 _this->bandwidth = {0}", _this->bandwidth);
                    _this->setBandwidth(bw);
                    gui::mainWindow.setbandwidth(_this->bandwidth);
                }
                if (_this->isServer)
                    gui::mainWindow.setUpdateMenuSnd2Radio(true);
            }
        }
        else if (code == RADIO_IFACE_CMD_GET_BANDWIDTH && out)
        {
            float *_out = (float *)out;
            *_out = _this->bandwidth;
        }
        else if (code == RADIO_IFACE_CMD_SET_BANDWIDTH && in)
        {
            double *_in = (double *)in;
            if (_this->bandwidthLocked)
            {
                return;
            }
            float bw = (float)(*_in);
            // flog::info("CMD_SET_BANDWIDTH bw = {0}, _this->bandwidth {1}", bw, _this->bandwidth);
            if (bw != _this->bandwidth)
            {
                flog::info("CMD_SET_BANDWIDTH 2 bw = {0}", bw);
                _this->setBandwidth(bw);

                gui::mainWindow.setbandwidth(_this->bandwidth);
                if (_this->isServer)
                    gui::mainWindow.setUpdateMenuSnd2Radio(true);
            }
        }
        else if (code == RADIO_IFACE_CMD_SET_SNAPINTERVAL && in)
        {
            int *_in = (int *)in;
            // if (_this->bandwidthLocked) { return; }
            int snap = (*_in);
            if (snap != _this->snapInterval)
            {
                _this->snapInterval = (*_in);
                for (int i = 0; i < _this->snapintervalsList.size(); i++)
                {
                    if (_this->snapintervalsList[i] >= _this->snapInterval)
                    {
                        _this->snapIntervalId = i;
                        // flog::info("TRACE. snapInterval = {0}, snapIntervalId ={1}!", _this->snapInterval, _this->snapIntervalId);
                        break;
                    }
                }
                // flog::info("962 _this->snapInterval ={0}", _this->snapInterval);
                _this->vfo->setSnapInterval(_this->snapInterval);
                gui::mainWindow.setsnapInterval(_this->snapInterval);
                gui::mainWindow.setsnapIntervalId(_this->snapIntervalId);
                if (_this->isServer)
                    gui::mainWindow.setUpdateMenuSnd2Radio(true); //_this->update_menu
                // flog::info("\nsetUpdateMenuSnd2Radio(true), _this->snapInterval {0}", _this->snapInterval);
            }
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_ENABLED && out)
        {
            bool *_out = (bool *)out;
            *_out = _this->squelchEnabled;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_ENABLED && in)
        {
            bool *_in = (bool *)in;
            _this->setSquelchEnabled(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_LEVEL && out)
        {
            float *_out = (float *)out;
            *_out = _this->squelchLevel;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_LEVEL && in)
        {
            float *_in = (float *)in;
            _this->setSquelchLevel(*_in);
        }
        else
        {
            // return;
            switch (code)
            {
            case RADIO_IFACE_CMD_ADD_TO_IFCHAIN:
                _this->ifChain.addBlock((dsp::Processor<dsp::complex_t, dsp::complex_t> *)in, false);
                return;
            case RADIO_IFACE_CMD_ADD_TO_AFCHAIN:
                _this->afChain.addBlock((dsp::Processor<dsp::stereo_t, dsp::stereo_t> *)in, false);
                return;
            case RADIO_IFACE_CMD_REMOVE_FROM_IFCHAIN:
                _this->ifChain.removeBlock((dsp::Processor<dsp::complex_t, dsp::complex_t> *)in, [=](dsp::stream<dsp::complex_t> *out)
                                           { _this->selectedDemod->setInput(out); });
                return;
            case RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN:
                // _this->afChain.removeBlock((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, [=](dsp::stream<dsp::stereo_t>* out) { _this->afsplitter.setInput(out); });
                return;
            case RADIO_IFACE_CMD_ENABLE_IN_IFCHAIN:
                _this->ifChain.setBlockEnabled((dsp::Processor<dsp::complex_t, dsp::complex_t> *)in, true, [=](dsp::stream<dsp::complex_t> *out)
                                               { _this->selectedDemod->setInput(out); });
                return;
            case RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN:
                // _this->afChain.setBlockEnabled((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, true, [=](dsp::stream<dsp::stereo_t>* out) { _this->afsplitter.setInput(out); });
                return;
            case RADIO_IFACE_CMD_DISABLE_IN_IFCHAIN:
                _this->ifChain.setBlockEnabled((dsp::Processor<dsp::complex_t, dsp::complex_t> *)in, false, [=](dsp::stream<dsp::complex_t> *out)
                                               { _this->selectedDemod->setInput(out); });
                return;
            case RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN:
                //  _this->afChain.setBlockEnabled((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, false, [=](dsp::stream<dsp::stereo_t>* out) { _this->afsplitter.setInput(out); });
                return;
            }
        }
        /// flog::info("TRACE 984. snapInterval ... _this->snapIntervalId {0} !", _this->snapIntervalId);

        // Success
        return;
    }

    // Handlers
    EventHandler<int> onUserChangedDemodulatorHandler;
    EventHandler<double>
        onUserChangedBandwidthHandler;
    EventHandler<float> srChangeHandler;
    EventHandler<dsp::stream<dsp::complex_t> *> ifChainOutputChanged;
    EventHandler<dsp::stream<dsp::stereo_t> *> afChainOutputChanged;

    VFOManager::VFO *vfo = NULL;

    // IF chain
    dsp::chain<dsp::complex_t> ifChain;
    dsp::noise_reduction::NoiseBlanker nb;
    dsp::noise_reduction::FMIF fmnr;
    dsp::noise_reduction::Squelch squelch;

    // Audio chain
    dsp::stream<dsp::stereo_t> dummyAudioStream;
    dsp::stream<dsp::complex_t> dummyIQStream;
    dsp::chain<dsp::stereo_t> afChain;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    dsp::filter::Deemphasis<dsp::stereo_t> deemp;

    SinkManager::Stream stream;

    demod::Demodulator *selectedDemod = NULL;

    OptionList<std::string, DeemphasisMode> deempModes;
    OptionList<std::string, IFNRPreset> ifnrPresets;

    double audioSampleRate = 48000.0;
    float minBandwidth;
    float maxBandwidth;
    float bandwidth = 2500;
    float bandwidthIQ = 50000.0;
    // int bandwidth;
    bool bandwidthLocked;
    int snapInterval;
    int selectedDemodID = 1;
    bool postProcEnabled;

    bool squelchEnabled = false;
    float squelchLevel;

    int deempId = 0;
    bool deempAllowed;

    bool FMIFNRAllowed;
    bool FMIFNREnabled = false;
    int fmIFPresetId;

    bool notchEnabled = false;
    float notchPos = 0;
    float notchWidth = 500;

    bool nbAllowed;
    bool nbEnabled = false;
    float nbLevel = 10.0f;

    const double MIN_NB = 1.0;
    const double MAX_NB = 10.0;
    const double MIN_SQUELCH = -100.0;
    const double MAX_SQUELCH = 0.0;

    std::vector<uint32_t> snapintervalsList;
    int snapIntervalId = 0;
    std::vector<uint32_t> bandwidthsList;
    int bandwidthId = 0;
    const char *snapintervalsListTxt = "1\0 2.5\0 5\0 6.25\0 10\0 12.5\0 20\0 25\0 30\0 50\0 100\0";
    const char *snapintervalsListTxtAzlClnt = "1\0 2\0 3\0 4\0 5\0 10\0 20\0 30\0 43\0 50\0 100\0";
    const char *bandwidthsListTxt = " 1\0 2.1\0 4\0 6.25\0 12.5\0 25\0 50\0 100\0 250\0"; // 250\0
    int radioMode = 0;
    int clnt_mode = -1;
    std::string source;
    bool enabled = true;

    bool isARM = false;
    bool isServer = false;
    std::string currSource;
    std::thread workerThread;
    bool update_menu = false;
    std::mutex Mtx;

    int baseband_band = 1000000;
    int curr_bb = 0;

    bool iqBlockAdded = false;
    bool Admin = false;

    std::atomic<int> requestedDemodId{-1};
    // НОВОЕ ПОЛЕ: Указатель на демодулятор, ожидающий удаления
    demod::Demodulator *demodToKill = NULL;
    std::atomic<bool> pleaseStop{false};
};
