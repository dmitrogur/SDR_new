#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <airspy.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "airspy_source",
    /* Description:     */ "Airspy source module for Aster",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1};

ConfigManager config;

class AirspySourceModule : public ModuleManager::Instance
{
public:
    AirspySourceModule(std::string name)
    {
        this->name = name;

        airspy_init();

        sampleRate = MAIN_SAMPLE_RATE; // 10000000.0;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();
        if (sampleRateList.size() > 0)
        {
            sampleRate = sampleRateList[0];
        }

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
            // source = core::configManager.conf["source"];
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            radioMode = 0;
        }
        if (core::configManager.conf["IsServer"] == true)
            isServer = true;
        else
            isServer = false;
        core::configManager.release();
        if (radioMode == 2)
        {
            AirSpyName = "Азалія-сервер";
        }
        else
        {
            AirSpyName = "Airspy";
        }
        flog::info("AirSpyName = {0}", AirSpyName);

        // Select device from config
        config.acquire();
        std::string devSerial = config.conf["device"];
        double def_freq = core::configManager.conf["frequency"];
        config.release();
        selectByString(devSerial);

        sigpath::sourceManager.registerSource(AirSpyName, &handler);
        core::modComManager.registerInterface(AirSpyName, name, moduleInterfaceHandler, this);
    }

    ~AirspySourceModule()
    {
        if (workerInfoThread.joinable())
        {
            workerInfoThread.join();
        }
        stop(this);
        sigpath::sourceManager.unregisterSource(AirSpyName); // Airspy
        airspy_exit();
    }

    void postInit()
    {
        if (isServer)
        { // isARM ||
            workerInfoThread = std::thread(&AirspySourceModule::workerInfo, this);
        }
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

    void refresh()
    {
#ifndef __ANDROID__
        devList.clear();
        devListTxt = "";

        uint64_t serials[256];
        int n = airspy_list_devices(serials, 256);

        char buf[1024];
        for (int i = 0; i < n; i++)
        {
            sprintf(buf, "%016" PRIX64, serials[i]);
            devList.push_back(serials[i]);
            devListTxt += buf;
            devListTxt += '\0';
        }
#else
        // Check for device presence
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::AIRSPY_VIDPIDS);
        if (devFd < 0)
        {
            return;
        }

        // Get device info
        std::string fakeName = "Airspy USB";
        devList.push_back(0xDEADBEEF);
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst()
    {
        if (devList.size() != 0)
        {
            selectBySerial(devList[0]);
            devId = 0;
        }
    }

    void selectByString(std::string serial)
    {
        char buf[1024];
        for (int i = 0; i < devList.size(); i++)
        {
            sprintf(buf, "%016" PRIX64, devList[i]);
            std::string str = buf;
            if (serial == str)
            {
                selectBySerial(devList[i]);
                flog::info("TRACE devId {0}:{1} selectByString serial == str", i, devList[i]);
                devId = i;
                return;
            }
        }
        selectFirst();
    }

    void selectBySerial(uint64_t serial)
    {
        airspy_device *dev;
        try
        {
#ifndef __ANDROID__
            int err = airspy_open_sn(&dev, serial);
#else
            int err = airspy_open_fd(&dev, devFd);
#endif
            if (err != 0)
            {
                char buf[1024];
                sprintf(buf, "%016" PRIX64, serial);
                flog::error("Could not open Airspy {0}", buf);
                selectedSerial = 0;
                return;
            }
        }
        catch (std::exception e)
        {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, serial);
            flog::error("Could not open Airspy {0}", buf);
        }
        // flog::error("serial {0}", serial);

        selectedSerial = serial;

        uint32_t sampleRates[256];
        airspy_get_samplerates(dev, sampleRates, 0);
        int n = sampleRates[0];
        airspy_get_samplerates(dev, sampleRates, n);
        sampleRateList.clear();
        sampleRateListTxt = "";
        for (int i = 0; i < n; i++)
        {
            sampleRateList.push_back(sampleRates[i]);
            sampleRateListTxt += getBandwdithScaled(sampleRates[i]);
            sampleRateListTxt += '\0';
        }

        char buf[1024];
        sprintf(buf, "%016" PRIX64, serial);
        selectedSerStr = std::string(buf);

        // Load config here
        config.acquire();
        bool created = false;
        if (!config.conf["devices"].contains(selectedSerStr))
        {
            created = true;
            config.conf["devices"][selectedSerStr]["sampleRate"] = 8000000; // 10000000;
            config.conf["devices"][selectedSerStr]["gainMode"] = 0;
            config.conf["devices"][selectedSerStr]["sensitiveGain"] = 0;
            config.conf["devices"][selectedSerStr]["linearGain"] = 0;
            config.conf["devices"][selectedSerStr]["lnaGain"] = 0;
            config.conf["devices"][selectedSerStr]["mixerGain"] = 0;
            config.conf["devices"][selectedSerStr]["vgaGain"] = 0;
            config.conf["devices"][selectedSerStr]["lnaAgc"] = false;
            config.conf["devices"][selectedSerStr]["mixerAgc"] = false;
            config.conf["devices"][selectedSerStr]["biasT"] = false;
        }

        // Load sample rate
        srId = 0;
        sampleRate = MAIN_SAMPLE_RATE; // sampleRate 8000000;// sampleRateList[0];
        /*
        if (config.conf["devices"][selectedSerStr].contains("sampleRate")) {
            int selectedSr = config.conf["devices"][selectedSerStr]["sampleRate"];
            for (int i = 0; i < sampleRateList.size(); i++) {
                if (sampleRateList[i] == selectedSr) {
                    srId = i;
                    sampleRate = selectedSr;
                    break;
                }
            }
        }
        */

        // Load gains
        if (config.conf["devices"][selectedSerStr].contains("gainMode"))
        {
            gainMode = config.conf["devices"][selectedSerStr]["gainMode"];
        }
        if (radioMode == 2)
        {
            // if(gainMode!=1) {
            gainMode = 1;
            linearGain = 11;
            config.conf["devices"][selectedSerStr]["gainMode"] = gainMode;
            config.conf["devices"][selectedSerStr]["linearGain"] = linearGain;
            // }
        }
        if (config.conf["devices"][selectedSerStr].contains("sensitiveGain"))
        {
            sensitiveGain = config.conf["devices"][selectedSerStr]["sensitiveGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("linearGain"))
        {
            linearGain = config.conf["devices"][selectedSerStr]["linearGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("lnaGain"))
        {
            lnaGain = config.conf["devices"][selectedSerStr]["lnaGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("mixerGain"))
        {
            mixerGain = config.conf["devices"][selectedSerStr]["mixerGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("vgaGain"))
        {
            vgaGain = config.conf["devices"][selectedSerStr]["vgaGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("lnaAgc"))
        {
            lnaAgc = config.conf["devices"][selectedSerStr]["lnaAgc"];
        }
        if (config.conf["devices"][selectedSerStr].contains("mixerAgc"))
        {
            mixerAgc = config.conf["devices"][selectedSerStr]["mixerAgc"];
        }

        // Load Bias-T
        if (config.conf["devices"][selectedSerStr].contains("biasT"))
        {
            biasT = config.conf["devices"][selectedSerStr]["biasT"];
        }
        config.release(created);

        airspy_close(dev);
    }

private:
    std::string getBandwdithScaled(double bw)
    {
        char buf[1024];
        if (bw >= 1000000.0)
        {
            sprintf(buf, "%.1lfМГц", bw / 1000000.0);
        }
        else if (bw >= 1000.0)
        {
            sprintf(buf, "%.1lfKГц", bw / 1000.0);
        }
        else
        {
            sprintf(buf, "%.1lfГц", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        core::setInputSampleRate(_this->sampleRate);
        // gui::mainWindow.setGainMode(_this->gainMode);
        // gui::mainWindow.setLinearGain(_this->linearGain);

        gui::mainWindow.setlnaGain(_this->lnaGain);
        gui::mainWindow.setvgaGain(_this->vgaGain);
        gui::mainWindow.setmixerGain(_this->mixerGain);
        gui::mainWindow.setlinearGain(_this->linearGain);
        gui::mainWindow.setsensitiveGain(_this->sensitiveGain);
        gui::mainWindow.setgainMode(_this->gainMode);
        gui::mainWindow.setlnaAgc(_this->lnaAgc);
        gui::mainWindow.setmixerAgc(_this->mixerAgc);
        gui::mainWindow.setselect(_this->select);
        gui::mainWindow.set_updateLinearGain(_this->_updateLinearGain);
        _this->update_menu = true;
        gui::mainWindow.setUpdateMenuSnd(true);

        flog::info("AirspySourceModule '{0}' {1}: Menu Select!", _this->name, _this->sampleRate);
        _this->select = true;
    }

    static void menuDeselected(void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        flog::info("AirspySourceModule '{0}': Menu Deselect!", _this->name);
        _this->select = false;
    }

    static void start(void *ctx)
    {
        flog::info(" start");
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        if (_this->running)
        {
            return;
        }
        //_this->selectBySerial(0);
        // usleep(100);
        if (_this->devList.size() == 0)
        {
            flog::error("Error Tried to start Airspy!");
            return;
        }
        try
        {
            flog::info(" start, _this->devId {0}, {1}", _this->devId, _this->devList[_this->devId]);
        }
        catch (...)
        {
            flog::error("Error Tried to start Airspy!");
            return;
        }

        try
        {
            auto &dev = _this->devList[_this->devId];
        }
        catch (...)
        {
            flog::error("Error Tried to start Airspy!");
            return;
        }
        // flog::error(" {0}", _this->devList[_this->devId]);
        _this->selectBySerial(_this->devList[_this->devId]);

        if (_this->selectedSerial == 0)
        {
            flog::error("Tried to start Airspy source with null serial");
            return;
        }

#ifndef __ANDROID__
        int err = airspy_open_sn(&_this->openDev, _this->selectedSerial);
#else
        int err = airspy_open_fd(&_this->openDev, _this->devFd);
#endif
        if (err != 0)
        {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, _this->selectedSerial);
            flog::error("Could not open Airspy {0}", buf);
            return;
        }

        airspy_set_samplerate(_this->openDev, _this->sampleRateList[_this->srId]);
        airspy_set_freq(_this->openDev, _this->freq);

        if (_this->gainMode == 0)
        {
            airspy_set_lna_agc(_this->openDev, 0);
            airspy_set_mixer_agc(_this->openDev, 0);
            airspy_set_sensitivity_gain(_this->openDev, _this->sensitiveGain);
            gui::mainWindow.setsensitiveGain(_this->sensitiveGain);
        }
        else if (_this->gainMode == 1)
        {
            airspy_set_lna_agc(_this->openDev, 0);
            airspy_set_mixer_agc(_this->openDev, 0);
            airspy_set_linearity_gain(_this->openDev, _this->linearGain);
        }
        else if (_this->gainMode == 2)
        {
            if (_this->lnaAgc)
            {
                airspy_set_lna_agc(_this->openDev, 1);
            }
            else
            {
                airspy_set_lna_agc(_this->openDev, 0);
                airspy_set_lna_gain(_this->openDev, _this->lnaGain);
            }
            if (_this->mixerAgc)
            {
                airspy_set_mixer_agc(_this->openDev, 1);
            }
            else
            {
                airspy_set_mixer_agc(_this->openDev, 0);
                airspy_set_mixer_gain(_this->openDev, _this->mixerGain);
            }
            airspy_set_vga_gain(_this->openDev, _this->vgaGain);
        }

        airspy_set_rf_bias(_this->openDev, _this->biasT);

        airspy_start_rx(_this->openDev, callback, _this);

        _this->running = true;
        flog::info("AirspySourceModule '{0}': Start! gui::mainWindow.getGainMode() {1}", _this->name, gui::mainWindow.getgainMode());
    }

    static void stop(void *ctx)
    {
        flog::info(" stop");
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        if (!_this->running)
        {
            return;
        }
        _this->running = false;
        _this->stream.stopWriter();
        airspy_close(_this->openDev);
        _this->stream.clearWriteStop();
        flog::info("AirspySourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double _freq, void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        /*
        if (_this->isServer && _this->freq != _freq) {
            flog::info("___frequency {0}, old_freq {1}", _freq, _this->freq);
            // gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        }
        */

        if (_this->running)
        {
            airspy_set_freq(_this->openDev, _freq);
        }
        _this->freq = _freq;
        //        flog::info("AirspySourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void workerInfo(void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        // if (currSource != SOURCE_ARM) return;
        std::mutex InfoMtx;
        while (true)
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                _this->update_menu = gui::mainWindow.getUpdateMenuRcv();

                if (_this->update_menu)
                {
                    gui::mainWindow.setUpdateMenuRcv(false);
                    // if (SmGui::RadioButton(CONCAT("Лінійн.##_airspy_gm_", _this->name), _this->gainMode == 1)) {
                    bool updt_cfg = false;

                    if (_this->gainMode != gui::mainWindow.getgainMode())
                    {
                        updt_cfg = true;
                    }
                    if (_this->mixerAgc != gui::mainWindow.getmixerAgc())
                    {
                        _this->mixerAgc = gui::mainWindow.getmixerAgc();
                    }

                    _this->gainMode = gui::mainWindow.getgainMode();
                    if (_this->gainMode == 1)
                    {
                        if (_this->linearGain != gui::mainWindow.getlinearGain())
                        {
                            _this->linearGain = gui::mainWindow.getlinearGain();
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["linearGain"] = _this->linearGain;
                            config.release(true);
                        };
                        if (_this->running)
                        {
                            airspy_set_lna_agc(_this->openDev, 0);
                            airspy_set_mixer_agc(_this->openDev, 0);
                            airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                            airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                        }
                        if (updt_cfg && _this->selectedSerStr != "")
                        {
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["gainMode"] = 1;
                            config.release(true);
                        }
                    }
                    else if (_this->gainMode == 2)
                    {
                        if (_this->lnaGain != gui::mainWindow.getlnaGain())
                        {
                            _this->lnaGain = gui::mainWindow.getlnaGain();
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["lnaGain"] = _this->lnaGain;
                            config.release(true);
                        }
                        if (_this->mixerGain != gui::mainWindow.getmixerGain())
                        {
                            _this->mixerGain = gui::mainWindow.getmixerGain();
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["mixerGain"] = _this->mixerGain;
                            config.release(true);
                        }
                        if (_this->vgaGain != gui::mainWindow.getvgaGain())
                        {
                            _this->vgaGain = gui::mainWindow.getvgaGain();
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["vgaGain"] = _this->vgaGain;
                            config.release(true);
                        }
                        if (_this->running)
                        {
                            if (_this->lnaAgc)
                            {
                                airspy_set_lna_agc(_this->openDev, 1);
                            }
                            else
                            {
                                airspy_set_lna_agc(_this->openDev, 0);
                                airspy_set_lna_gain(_this->openDev, _this->lnaGain);
                            }
                            if (_this->mixerAgc)
                            {
                                airspy_set_mixer_agc(_this->openDev, 1);
                            }
                            else
                            {
                                airspy_set_mixer_agc(_this->openDev, 0);
                                airspy_set_mixer_gain(_this->openDev, _this->mixerGain);
                            }
                            airspy_set_vga_gain(_this->openDev, _this->vgaGain);
                        }
                        if (updt_cfg && _this->selectedSerStr != "")
                        {
                            config.acquire();
                            config.conf["devices"][_this->selectedSerStr]["gainMode"] = 2;
                            config.release(true);
                        }
                    }
                    _this->sensitiveGain = gui::mainWindow.getsensitiveGain();
                    _this->select = gui::mainWindow.getselect();
                    _this->_updateLinearGain = gui::mainWindow.get_updateLinearGain();
                    flog::info("TRACE RCV menuHandler 1 _this->update_menu {0}, gui::mainWindow.getlinearGain() {1} _this->vgaGain {2}", _this->update_menu, gui::mainWindow.getlinearGain(), _this->vgaGain);
                }
                _this->update_menu = false;
            }
        }
    }
    static void menuHandler(void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;

        if (_this->running)
        {
            SmGui::BeginDisabled();
        }
        _this->update_menu = false;
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_airspy_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str()))
        {
            flog::info("TRACE devID {0}:{1} menuHandler", _this->devId, _this->devList[_this->devId]);
            _this->selectBySerial(_this->devList[_this->devId]);
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerStr != "")
            {
                config.acquire();
                config.conf["device"] = _this->selectedSerStr;
                config.release(true);
            }
            _this->update_menu = true;
        }

        if (_this->running)
        {
            SmGui::EndDisabled();
        }
        if (!_this->Admin)
        {
            if (_this->gainMode != 1)
            {
                _this->gainMode = 1;
                // gui::mainWindow.setgainMode(_this->gainMode);
                if (_this->running)
                {
                    airspy_set_lna_agc(_this->openDev, 0);
                    airspy_set_mixer_agc(_this->openDev, 0);
                    airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["gainMode"] = 1;
                    config.release(true);
                }
                _this->update_menu = true;
            }
        }
        else
        {

            SmGui::BeginGroup();
            SmGui::Columns(2, CONCAT("AirspyGainModeColumns##_", _this->name), false);

            SmGui::ForceSync();
            if (SmGui::RadioButton(CONCAT("Лінійн.##_airspy_gm_", _this->name), _this->gainMode == 1))
            {
                _this->gainMode = 1;
                // gui::mainWindow.setgainMode(_this->gainMode);
                if (_this->running)
                {
                    airspy_set_lna_agc(_this->openDev, 0);
                    airspy_set_mixer_agc(_this->openDev, 0);
                    airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["gainMode"] = 1;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            SmGui::NextColumn();
            SmGui::ForceSync();
            if (SmGui::RadioButton(CONCAT("Довільн.##_airspy_gm_", _this->name), _this->gainMode == 2))
            {
                _this->gainMode = 2;
                // gui::mainWindow.setGainMode(_this->gainMode);
                if (_this->running)
                {
                    if (_this->lnaAgc)
                    {
                        airspy_set_lna_agc(_this->openDev, 1);
                    }
                    else
                    {
                        airspy_set_lna_agc(_this->openDev, 0);
                        airspy_set_lna_gain(_this->openDev, _this->lnaGain);
                    }
                    if (_this->mixerAgc)
                    {
                        airspy_set_mixer_agc(_this->openDev, 1);
                    }
                    else
                    {
                        airspy_set_mixer_agc(_this->openDev, 0);
                        airspy_set_mixer_gain(_this->openDev, _this->mixerGain);
                    }
                    airspy_set_vga_gain(_this->openDev, _this->vgaGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["gainMode"] = 2;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            SmGui::Columns(1, CONCAT("EndAirspyGainModeColumns##_", _this->name), false);
            SmGui::EndGroup();
        }
        // Gain menus

        if (_this->gainMode == 0)
        {
            SmGui::LeftLabel("Підс.");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_airspy_sens_gain_", _this->name), &_this->sensitiveGain, 0, 21))
            {
                if (_this->running)
                {
                    airspy_set_sensitivity_gain(_this->openDev, _this->sensitiveGain);
                    gui::mainWindow.setsensitiveGain(_this->sensitiveGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["sensitiveGain"] = _this->sensitiveGain;
                    config.release(true);
                }
                _this->update_menu = true;
            }
        }
        else if (_this->gainMode == 1)
        {
            if (_this->_updateLinearGain)
            {
                if (_this->running)
                {
                    airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["linearGain"] = _this->linearGain;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            _this->_updateLinearGain = false;

            SmGui::LeftLabel("Підс.");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_airspy_lin_gain_", _this->name), &_this->linearGain, 0, 21))
            {
                // gui::mainWindow.setGainMode(_this->gainMode);
                // gui::mainWindow.setLinearGain(_this->linearGain);

                if (_this->running)
                {
                    airspy_set_linearity_gain(_this->openDev, _this->linearGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["linearGain"] = _this->linearGain;
                    config.release(true);
                }
                _this->update_menu = true;
                gui::mainWindow.setlinearGain(_this->linearGain);
            }
        }
        else if (_this->gainMode == 2)
        {
            // TODO: Switch to a table for alignment
            if (_this->lnaAgc)
            {
                SmGui::BeginDisabled();
            }
            SmGui::LeftLabel("Підс. МШП");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_airspy_lna_gain_", _this->name), &_this->lnaGain, 0, 15))
            {
                if (_this->running)
                {
                    airspy_set_lna_gain(_this->openDev, _this->lnaGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["lnaGain"] = _this->lnaGain;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            if (_this->lnaAgc)
            {
                SmGui::EndDisabled();
            }

            if (_this->mixerAgc)
            {
                SmGui::BeginDisabled();
            }
            SmGui::LeftLabel("Підс. Зміш.");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_airspy_mix_gain_", _this->name), &_this->mixerGain, 0, 15))
            {
                if (_this->running)
                {
                    airspy_set_mixer_gain(_this->openDev, _this->mixerGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["mixerGain"] = _this->mixerGain;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            if (_this->mixerAgc)
            {
                SmGui::EndDisabled();
            }

            SmGui::LeftLabel("Підс. VGA");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_airspy_vga_gain_", _this->name), &_this->vgaGain, 0, 15))
            {
                if (_this->running)
                {
                    airspy_set_vga_gain(_this->openDev, _this->vgaGain);
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["vgaGain"] = _this->vgaGain;
                    config.release(true);
                }
                gui::mainWindow.setvgaGain(_this->vgaGain);
                _this->update_menu = true;
            }

            // AGC Control
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("АРП МШП##_airspy_", _this->name), &_this->lnaAgc))
            {
                if (_this->running)
                {
                    if (_this->lnaAgc)
                    {
                        airspy_set_lna_agc(_this->openDev, 1);
                    }
                    else
                    {
                        airspy_set_lna_agc(_this->openDev, 0);
                        airspy_set_lna_gain(_this->openDev, _this->lnaGain);
                    }
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["lnaAgc"] = _this->lnaAgc;
                    config.release(true);
                }
                _this->update_menu = true;
            }
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("АРП Зміш.##_airspy_", _this->name), &_this->mixerAgc))
            {
                if (_this->running)
                {
                    if (_this->mixerAgc)
                    {
                        airspy_set_mixer_agc(_this->openDev, 1);
                    }
                    else
                    {
                        airspy_set_mixer_agc(_this->openDev, 0);
                        airspy_set_mixer_gain(_this->openDev, _this->mixerGain);
                    }
                }
                if (_this->selectedSerStr != "")
                {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["mixerAgc"] = _this->mixerAgc;
                    config.release(true);
                }
                _this->update_menu = true;
            }
        }
        if (_this->update_menu == true)
        {
            flog::info("TRACE SND menuHandler 2 _this->update_menu {0}, _this->linearGain {1}", _this->update_menu, _this->linearGain);
            gui::mainWindow.setlnaGain(_this->lnaGain);
            gui::mainWindow.setvgaGain(_this->vgaGain);
            gui::mainWindow.setmixerGain(_this->mixerGain);
            gui::mainWindow.setlinearGain(_this->linearGain);
            gui::mainWindow.setsensitiveGain(_this->sensitiveGain);
            gui::mainWindow.setgainMode(_this->gainMode);
            gui::mainWindow.setlnaAgc(_this->lnaAgc);
            gui::mainWindow.setmixerAgc(_this->mixerAgc);
            gui::mainWindow.setselect(_this->select);
            gui::mainWindow.set_updateLinearGain(_this->_updateLinearGain);
            gui::mainWindow.setUpdateMenuSnd(true); //_this->update_menu
        }
    }

    static int callback(airspy_transfer_t *transfer)
    {
        AirspySourceModule *_this = (AirspySourceModule *)transfer->ctx;
        memcpy(_this->stream.writeBuf, transfer->samples, transfer->sample_count * sizeof(dsp::complex_t));
        if (!_this->stream.swap(transfer->sample_count))
        {
            return -1;
        }
        return 0;
    }

    static void moduleInterfaceHandler(int code, void *in, void *out, void *ctx)
    {
        AirspySourceModule *_this = (AirspySourceModule *)ctx;
        if (code == AIRSPY_IFACE_CMD_GET_RECORDING)
        {
            ///           flog::info("IRSPY_IFACE_CMD_GET_RECORDING select= {0}, _this->running {1}", _this->select, _this->running);
            int *_out = (int *)out;
            // *_out = _this->running;

            if (_this->select == false)
            {
                *_out = 1;
            }
            else
            {
                if (_this->running)
                {
                    *_out = 1;
                }
                else
                {
                    *_out = 0;
                }
            }
        }
    }

    std::string name;
    airspy_device *openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    uint64_t selectedSerial = 0;
    std::string selectedSerStr = "";
    int devId = 0;
    int srId = 0;

    bool biasT = false;

    int lnaGain = 0;
    int vgaGain = 0;
    int mixerGain = 0;
    int linearGain = 0;
    int sensitiveGain = 0;

    int gainMode = 0;

    bool lnaAgc = false;
    bool mixerAgc = false;
    bool select = false;
    bool _updateLinearGain = false;
    bool update_menu = false;
#ifdef __ANDROID__
    int devFd = 0;
#endif

    std::vector<uint64_t> devList;
    std::string devListTxt;
    std::vector<uint32_t> sampleRateList;
    std::string sampleRateListTxt;

    std::string AirSpyName = "Airspy";

    int radioMode = 0;
    bool isServer = false;

    std::thread workerInfoThread;

    // std::string source = "Airspy";
    bool Admin = false;
};

MOD_EXPORT void _INIT_()
{
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/airspy_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name)
{
    return new AirspySourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance *instance)
{
    delete (AirspySourceModule *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
}