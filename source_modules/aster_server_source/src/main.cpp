#include "aster_server_client.h"
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <utils/optionlist.h>
#include <gui/dialogs/dialog_box.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "aster_server_source",
    /* Description:     */ "ASter Server source module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class AsterServerSourceModule : public ModuleManager::Instance {
public:
    AsterServerSourceModule(std::string name) {
        this->name = name;

        // Yeah no server-ception, sorry...
        if (core::args["server"].b()) { return; }

        // Initialize lists
        sampleTypeList.define("Int8", dsp::compression::PCM_TYPE_I8);
        sampleTypeList.define("Int16", dsp::compression::PCM_TYPE_I16);
        sampleTypeList.define("Float32", dsp::compression::PCM_TYPE_F32);
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        // Load config
        config.acquire();
        // std::string hostStr1 = config.conf["hostname1"];
        // strcpy(hostname, hostStr.c_str());
        // port = config.conf["port"];
        port1 = 4000;
        port2 = 5000;
        std::string hostStr1 = "localhost";
        std::string hostStr2 = "localhost";

        try {
            hostStr1 = config.conf["hostname1"];
            hostStr2 = config.conf["hostname2"];
            port1 = config.conf["port1"];
            port2 = config.conf["port2"];
        }
        catch (...) {
            std::cout << "Error config.conf 1" << std::endl;
        }
        strcpy(hostname1, hostStr1.c_str());
        strcpy(hostname2, hostStr2.c_str());

        config.release();

        sigpath::sourceManager.registerSource("Aster Server", &handler);
    }

    ~AsterServerSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Aster Server");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMГц", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKГц", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfГц", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        if (_this->client1)
            core::setInputSampleRate(_this->client1->getSampleRate());
        else if (_this->client2)
            core::setInputSampleRate(_this->client2->getSampleRate());
        if (_this->client1)
            gui::mainWindow.playButtonLocked = !(_this->client1 && _this->client1->isOpen());
        else
            gui::mainWindow.playButtonLocked = !(_this->client2 && _this->client2->isOpen());

        flog::info("AsterServerSourceModule '{0}': Menu Select! playButtonLocked {1}", _this->name, gui::mainWindow.playButtonLocked);
    }

    static void menuDeselected(void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        gui::mainWindow.playButtonLocked = false;
        flog::info("AsterServerSourceModule '{0}': Menu Deselect! ", _this->name);
    }

    static void start(void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        flog::info("_source '{0}'!", _this->_source);
        // Try to connect if not already connected
        if (_this->_source == 1) {
            if (!_this->client1) {
                _this->tryConnect1();
                if (!_this->client1) { return; }
            }

            // Set configuration
            _this->client1->setFrequency(_this->freq);
            _this->client1->start();
            _this->running1 = true;
        }

        if (_this->_source == 2) {
            if (_this->running2) { return; }
            if (!_this->client2) {
                _this->tryConnect2();
                if (!_this->client2) { return; }
            }
            // Set configuration
            _this->client2->setFrequency(_this->freq);
            _this->client2->start();
            _this->running2 = true;
        }

        flog::info("AsterServerSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        if (!_this->running1 && !_this->running2) { return; }

        if (_this->client1) {
            _this->client1->stop();
            _this->running1 = false;
        }
        if (_this->client2) {
            _this->client2->stop();
            _this->running2 = false;
        }

        flog::info("AsterServerSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        if (_this->running1 && _this->client1) {
            _this->client1->setFrequency(freq);
        }
        else

            if (_this->running2 && _this->client2) {
            _this->client2->setFrequency(freq);
        }

        _this->freq = freq;
        flog::info("AsterServerSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        AsterServerSourceModule* _this = (AsterServerSourceModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool connected1 = (_this->client1 && _this->client1->isOpen());
        // gui::mainWindow.playButtonLocked = !connected1;
        bool connected2 = (_this->client2 && _this->client2->isOpen());
        gui::mainWindow.playButtonLocked = !connected1 && !connected2;

        ImGui::GenericDialog("##aster_srv_src_err_dialog", _this->serverBusy, GENERIC_DIALOG_BUTTONS_OK, [=]() {
            ImGui::TextUnformatted("This server is already in use.");
        });

        //==============================================
        if (connected1) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##aster_srv_srv0_host_", _this->name), _this->hostname1, 1023)) {
            try {
                config.acquire();
                config.conf["hostname1"] = std::string(_this->hostname1);
                config.release(true);
            }
            catch (...) {
                std::cout << "Error config.conf 2" << std::endl;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##aster_srv_srv0_port_", _this->name), &_this->port1, 1, 10)) {
            try {
                config.acquire();
                config.conf["port1"] = _this->port1;
                config.release(true);
            }
            catch (...) {
                std::cout << "Error config.conf 3" << std::endl;
            }
        }
        if (connected1) { style::endDisabled(); }

        if (_this->running1) { style::beginDisabled(); }
        if (!connected1 && ImGui::Button("З'єднати##aster_srv0_source", ImVec2(menuWidth, 0))) {
            _this->_source = 1;
            _this->tryConnect1();
        }
        else if (connected1 && ImGui::Button("Відключити##aster_srv0_source", ImVec2(menuWidth, 0))) {
            _this->_source = 0;
            _this->client1->close();
        }

        if (_this->running1) { style::endDisabled(); }

        if (connected1) {
            if (ImGui::Checkbox("Компресія##aster_srv0_compress", &_this->compression)) {
                _this->client1->setCompression(_this->compression);

                // Save config
                config.acquire();
                config.conf["servers"][_this->devConfName]["compression"] = _this->compression;
                config.release(true);
            }

            // Calculate datarate
            _this->frametimeCounter1 += ImGui::GetIO().DeltaTime;
            if (_this->frametimeCounter1 >= 0.2f) {
                _this->datarate1 = ((float)_this->client1->bytes / (_this->frametimeCounter1 * 1024.0f * 1024.0f)) * 8;
                _this->frametimeCounter1 = 0;
                _this->client1->bytes = 0;
            }

            ImGui::TextUnformatted("Статус:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Підключено (%.3f Мбіт/с)", _this->datarate1);

            ImGui::CollapsingHeader("Джерело", ImGuiTreeNodeFlags_DefaultOpen);

            _this->client1->showMenu();
        }
        else {
            ImGui::TextUnformatted("Статус:");
            ImGui::SameLine();
            ImGui::TextUnformatted("Не підключено (--.--- Мбіт/с)");
        }


        //==============================================

        // connected = (_this->client2 && _this->client2->isOpen());
        // gui::mainWindow.playButtonLocked = !connected;
        // DMH +++
        {
            if (connected2) { style::beginDisabled(); }

            if (ImGui::InputText(CONCAT("##aster_srv_srv1_host_", _this->name), _this->hostname2, 1023)) {
                config.acquire();
                config.conf["hostname2"] = std::string(_this->hostname2);
                config.release(true);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt(CONCAT("##aster_srv_srv1_port_", _this->name), &_this->port2, 0, 0)) {
                config.acquire();
                config.conf["port2"] = _this->port2;
                config.release(true);
            }
            if (connected2) { style::endDisabled(); }

            if (_this->running2) { style::beginDisabled(); }
            if (!connected2 && ImGui::Button("З'єднати##aster_srv1_source", ImVec2(menuWidth, 0))) {
                _this->_source = 2;
                _this->tryConnect2();
            }
            else if (connected2 && ImGui::Button("Відключити##aster_srv1_source", ImVec2(menuWidth, 0))) {
                _this->_source = 0;
                _this->client2->close();
            }
            if (_this->running2) { style::endDisabled(); }

            if (connected2) {
                if (ImGui::Checkbox("Компресія##aster_srv1_compress", &_this->compression)) {
                    _this->client2->setCompression(_this->compression);
                    // Save config
                    config.acquire();
                    config.conf["servers"][_this->devConfName]["compression"] = _this->compression;
                    config.release(true);
                }

                // Calculate datarate
                _this->frametimeCounter2 += ImGui::GetIO().DeltaTime;
                if (_this->frametimeCounter2 >= 0.2f) {
                    _this->datarate2 = ((float)_this->client2->bytes / (_this->frametimeCounter2 * 1024.0f * 1024.0f)) * 8;
                    _this->frametimeCounter2 = 0;
                    _this->client2->bytes = 0;
                }

                ImGui::TextUnformatted("Статус:");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Підключено (%.3f Мбіт/с)", _this->datarate2);

                ImGui::CollapsingHeader("Джерело", ImGuiTreeNodeFlags_DefaultOpen);

                _this->client2->showMenu();
            }
            else {
                ImGui::TextUnformatted("Статус:");
                ImGui::SameLine();
                ImGui::TextUnformatted("Не підключено (--.--- Мбіт/с)");
            }
        } // DMH >> +++
    }

    void tryConnect1() {
        try {
            if (client1) { client1.reset(); }
            client1 = server::connect(hostname1, port1, &stream);
            deviceInit1();
        }
        catch (std::exception e) {
            flog::error("Не вдалося підключитися до Aster: {0}", e.what());
            if (!strcmp(e.what(), "Сервер зайнятий")) { serverBusy = true; }
        }
    }

    void tryConnect2() {
        try {
            if (client2) { client2.reset(); }
            client2 = server::connect(hostname2, port2, &stream);
            deviceInit2();
        }
        catch (std::exception e) {
            flog::error("Не вдалося підключитися до Aster: {0}", e.what());
            if (!strcmp(e.what(), "Сервер зайнятий")) { serverBusy = true; }
        }
    }

    void deviceInit1() {
        // Generate the config name
        char buf[4096];
        sprintf(buf, "%s:%05d", hostname1, port1);
        devConfName = buf;

        // Load settings
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);
        if (config.conf["servers"][devConfName].contains("sampleType")) {
            std::string key = config.conf["servers"][devConfName]["sampleType"];
            if (sampleTypeList.keyExists(key)) { sampleTypeId = sampleTypeList.keyId(key); }
        }
        if (config.conf["servers"][devConfName].contains("compression")) {
            compression = config.conf["servers"][devConfName]["compression"];
        }

        // Set settings
        client1->setSampleType(sampleTypeList[sampleTypeId]);
        client1->setCompression(compression);
    }

    void deviceInit2() {
        // Generate the config name
        char buf[4096];
        sprintf(buf, "%s:%05d", hostname2, port2);
        devConfName = buf;

        // Load settings
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);
        if (config.conf["servers"][devConfName].contains("sampleType")) {
            std::string key = config.conf["servers"][devConfName]["sampleType"];
            if (sampleTypeList.keyExists(key)) { sampleTypeId = sampleTypeList.keyId(key); }
        }
        if (config.conf["servers"][devConfName].contains("compression")) {
            compression = config.conf["servers"][devConfName]["compression"];
        }

        // Set settings
        client2->setSampleType(sampleTypeList[sampleTypeId]);
        client2->setCompression(compression);
    }

    std::string name;
    bool enabled = true;
    bool running1 = false;
    bool running2 = false;

    double freq;
    bool serverBusy = false;

    float datarate1 = 0;
    float frametimeCounter1 = 0;
    float datarate2 = 0;
    float frametimeCounter2 = 0;

    char hostname1[1024];
    char hostname2[1024];

    int port1 = 50002;
    int port2 = 50004;

    std::string devConfName = "";

    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;

    OptionList<std::string, dsp::compression::PCMType> sampleTypeList;
    int sampleTypeId;
    bool compression = false;

    bool dummy = true;

    // server::Client client;
    server::Client client1;
    server::Client client2;
    int _source = 0;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    try {

        def["hostname1"] = "localhost";
        def["port1"] = 5259;

        def["hostname2"] = "localhost";
        def["port2"] = 5262;
    }
    catch (...) {
        std::cout << "Error config.conf 4" << std::endl;
    }

    def["servers"] = json::object();
    config.setPath(core::args["root"].s() + "/aster_server_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new AsterServerSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (AsterServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}