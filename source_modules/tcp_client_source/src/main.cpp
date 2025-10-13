#include <tcp_client.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "tcp_client_source",
    /* Description:     */ "TCP client source module for Malva",
    /* Author:          */ "DMH",
    /* Version:         */ 1, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class TCPClientSourceModule : public ModuleManager::Instance {
public:
    TCPClientSourceModule(std::string name) {
        this->name = name;

        // Create a list of sample rates
        /*
        for (int sr = 12000; sr < 200000; sr += 12000) {
            sampleRates.push_back(sr);
        }
        for (int sr = 11025; sr < 192000; sr += 11025) {
            sampleRates.push_back(sr);
        }
        sampleRates.push_back(250000);
        */
        sampleRates.push_back(12000);
        sampleRates.push_back(24000);
        sampleRates.push_back(48000);
        sampleRates.push_back(60000);
        sampleRates.push_back(72000);
        sampleRates.push_back(192000);
        sampleRates.push_back(250000);
        sampleRates.push_back(1000000);
        sampleRates.push_back(10000000);

        // Sort sample rate list
        std::sort(sampleRates.begin(), sampleRates.end(), [](double a, double b) { return (a < b); });

        // Define direct sampling modes
        directSamplingModes.define(0, "Disabled", 0);
        directSamplingModes.define(1, "I branch", 1);
        directSamplingModes.define(2, "Q branch", 2);

        // Select the default samplerate instead of id 0

        // Load config
        config.acquire();
        if (config.conf.contains("hostname")) {
            std::string hostStr = config.conf["hostname"];
            strcpy(ip, hostStr.c_str());
        }
        if (config.conf.contains("port")) {
            port = config.conf["port"];
        }
        if (config.conf.contains("sampleRate")) {
            // double sr = config.conf["sampleRate"];
            sampleRate = config.conf["sampleRate"];
        }
        if (config.conf.contains("directSamplingMode")) {
            int mode = config.conf["directSamplingMode"];
            if (directSamplingModes.keyExists(mode)) { directSamplingId = directSamplingModes.keyId(mode); }
        }
        if (config.conf.contains("ppm")) {
            ppm = config.conf["ppm"];
        }
        if (config.conf.contains("gainIndex")) {
            gain = config.conf["gainIndex"];
        }
        if (config.conf.contains("biasTee")) {
            biasTee = config.conf["biasTee"];
        }
        if (config.conf.contains("offsetTuning")) {
            offsetTuning = config.conf["offsetTuning"];
        }
        config.release();

        // Update samplerate
        // Generate text list for UI
        char buffer[128];
        int id = 0;
        int _48kId;
        bool found = false;
        for (auto sr : sampleRates) {
            sprintf(buffer, "%d", (int)sr);
            sampleRatesTxt += buffer;
            sampleRatesTxt += '\0';
            if (sr == sampleRate) {
                srId = id;
                found = true;
            }
            if (sr == 192000.0) { _48kId = id; }
            id++;
        }
        if (!found) {
            srId = _48kId;
            sampleRate = 192000.0;
        }
        flog::info("TCPClientSourceModule(std::string name) sampleRate {0}", sampleRate);           

        // Register source
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("Азалія-клієнт", &handler);
    }

    ~TCPClientSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Азалія-клієнт");
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
    static void menuSelected(void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;
        flog::info("TCPClientSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;
        flog::info("TCPClientSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;
        if (_this->running) { return; }

        core::setInputSampleRate(_this->sampleRate);
        if(!_this->clientInfo){
            // Connect to the serverInfo 
            try {
                _this->clientInfo = server::connectInfo(&_this->stream, _this->ip, _this->port);
                // Sync settings
                _this->clientInfo->setFrequency(_this->freq);
                _this->clientInfo->setSampleRate(_this->sampleRate);
            }
            catch (const std::exception& e) {
                flog::error("Could connect to TCP server INFO: {}", e.what());
                gui::mainWindow.setPlayState(false);
                 _this->running = false;
                return;
            }
        }    
        _this->client = NULL;

        /*
        try {
            _this->client = server::connect(&_this->stream, _this->ip, _this->port);
        }
        catch (const std::exception& e) {
            flog::error("Could connect to TCP server: {}", e.what());
            return;
        }
        */
        _this->running = true;

        bool connectedInfo = _this->connectedInfo() ; // _this->connected() &&
        flog::info("connected ==== connectedInfo {0}!!", connectedInfo);        

        if (connectedInfo) {       
            usleep(1000);     
            if(_this->clientInfo->getUpdate()) {
                double _freq = _this->clientInfo->getCurrFrequency();
                unsigned int _sampleRate = _this->clientInfo->getCurrSampleRate();
                _this->volLevel = _this->clientInfo->getVolLevel();
                _this->gainMode   = _this->clientInfo->getGainMode();
                _this->linearGain = _this->clientInfo->getLinearGain();

                flog::info("connected 1 _this->freq {0} != _freq = '{1}', _sampleRate: {2}!", _this->freq, _freq, _sampleRate);
                gui::freqSelect.setFrequency(_freq);
                gui::freqSelect.frequencyChanged = true;
                gui::waterfall.setCenterFrequency(_freq);

                if(1) {                
                    config.acquire();
                    config.conf["sampleRate"] = _this->sampleRate;
                    config.release(true);

                    // restart
                    if(_this->client) {
                        _this->client->close();
                        _this->client = NULL;
                        usleep(100);
                    } 
            
                    int id = 0;
                    bool found = false;
                    unsigned int _192Id  = 0;
                    for (auto sr : _this->sampleRates) {
                        if (sr == _sampleRate) {
                            _this->srId = id;
                            found = true;
                        }    
                        if (sr == 192000) { _192Id = id; }
                        id++;
                    }
                    if(!found) {
                        _this->srId = _192Id;
                        _sampleRate = 192000;
                    }
                
                    flog::info("connected_this->srId {0}!", _this->srId);                
                }
                _this->clientInfo->setUpdate(false);
                
                _this->freq = _freq;
                _this->sampleRate =_sampleRate;
            }

            if(!_this->client && _this->running) {
                try {
                    flog::info("!!! server::connect!");                
                    core::setInputSampleRate(_this->sampleRate);
                    _this->client = server::connect(&_this->stream, _this->ip, _this->port);
                }
                catch (const std::exception& e) {
                    flog::error("Could connect to TCP server: {}", e.what());
                }
            }
        }

        flog::info("TCPClientSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;
        if (!_this->running) { return; }
        if(_this->clientInfo) {
            _this->clientInfo->close();
            _this->clientInfo = NULL;
        }    
        if(_this->client){
            _this->client->close();
            _this->client = NULL;
        }    
        _this->running = false;
        flog::info("TCPClientSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;
        if (_this->running) {
            // _this->clientInfo->setFrequency(freq);
        }
        _this->freq = freq;
        flog::info("TCPClientSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        TCPClientSourceModule* _this = (TCPClientSourceModule*)ctx;

        bool connected = _this->connected() ; // _this->connected() &&
        bool connectedInfo = _this->connectedInfo() ; // _this->connected() &&

        // flog::info("connected ==== connectedInfo {0}!, connected = '{1}'!", connectedInfo, connected);        
        // gui::mainWindow.playButtonLocked = !connected;
        if (connectedInfo) {            
            if(_this->clientInfo->getUpdate()) {
                double _freq = _this->clientInfo->getCurrFrequency();
                unsigned int _sampleRate = _this->clientInfo->getCurrSampleRate();
                _this->volLevel   = _this->clientInfo->getVolLevel();
                _this->gainMode   = _this->clientInfo->getGainMode();
                _this->linearGain = _this->clientInfo->getLinearGain();

                if(_this->freq!=_freq) {
                    flog::info("connected 1 _this->freq {0} != _freq = '{1}', _sampleRate: {2}!", _this->freq, _freq, _sampleRate);

                    gui::freqSelect.setFrequency(_freq);
                    gui::freqSelect.frequencyChanged = true;
                    // sigpath::sourceManager.tune(frequency);
                    gui::waterfall.setCenterFrequency(_freq);
                }

                if(_this->sampleRate!=_sampleRate) {                
                    flog::info("connected 2 _this->sampleRate {0} !=_sampleRate {1}, _freq {2}!", _this->sampleRate, _sampleRate, _freq);

                    config.acquire();
                    config.conf["sampleRate"] = _this->sampleRate;
                    config.release(true);
                    // _this->client->updateStream();
                    // core::setInputSampleRate(_this->sampleRate);
                    // double finalBw = _sampleRate+(10*_sampleRate)/100;
                    //  gui::waterfall.setBandwidth(finalBw);
                    // gui::waterfall.setViewBandwidth(_sampleRate);
                    // gui::mainWindow.setViewBandwidthSlider(0.922);
                
                    // restart
                    if(_this->client) {
                        _this->client->close();
                        _this->client = NULL;
                        usleep(100);
                    } 
            
                    int id = 0;
                    bool found = false;
                    unsigned int _192Id  = 0;
                    for (auto sr : _this->sampleRates) {
                        if (sr == _sampleRate) {
                            _this->srId = id;
                            found = true;
                        }    
                        if (sr == 192000) { _192Id = id; }
                        id++;
                    }
                    if(!found) {
                        _this->srId = _192Id;
                        _sampleRate = 192000;
                    }
                
                    flog::info("connected_this->srId {0}!", _this->srId);                
                }
                _this->clientInfo->setUpdate(false);
                
                _this->freq = _freq;
                _this->sampleRate =_sampleRate;
            }

            if(!_this->client && _this->running) {
                try {
                    flog::info("!!! server::connect!");                
                    core::setInputSampleRate(_this->sampleRate);
                    _this->client = server::connect(&_this->stream, _this->ip, _this->port);
                }
                catch (const std::exception& e) {
                    flog::error("Could connect to TCP server: {}", e.what());
                }
            }
        } else {
            if(_this->running || gui::mainWindow.isPlaying()){
                gui::mainWindow.setPlayState(false);
            }    
        }

        if (_this->running) { SmGui::BeginDisabled(); }

        if (SmGui::InputText(CONCAT("##_ip_select_", _this->name), _this->ip, 1024)) {
            config.acquire();
            config.conf["hostname"] = std::string(_this->ip);
            config.release(true);
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_port_select_", _this->name), &_this->port, 0)) {
            config.acquire();
            config.conf["port"] = _this->port;
            config.release(true);
        }

        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtltcp_sr_", _this->name), &_this->srId,  _this->sampleRatesTxt.c_str())) {
            _this->sampleRate = _this->sampleRates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["sampleRate"] = _this->sampleRate;
            config.release(true);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        // ============================================
        SmGui::ForceSync();
                
        if (connectedInfo) {
            // if (_this->running) { style::beginDisabled(); }
            float menuWidth = ImGui::GetContentRegionAvail().x;

            if(!_this->clientInfo->server_status() && ImGui::Button("Старт (Азалія-Сервер)", ImVec2(menuWidth, 0))) {
                _this->clientInfo->start_server(true);
            }
            else if (_this->clientInfo->server_status() && ImGui::Button("Стоп (Азалія-Сервер)##tcp_srv_source", ImVec2(menuWidth, 0))) {
                _this->clientInfo->start_server(false);
            }
            
            ImGui::LeftLabel("Звук (Азалія-Сервер)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());            
            if (ImGui::SliderFloat("##_clnt_sqelch_lvl_", &_this->volLevel, 0.0f, 1.0f, "%0.2f")) {
                _this->clientInfo->setVolLevel(_this->volLevel, true);
            }

            if (_this->clientInfo->getGainMode() == 1) { // Лінійн. Підсилення РПП
                ImGui::LeftLabel("Підс. РПП (Азалія-Сервер)");
                SmGui::FillWidth();
                if (SmGui::SliderInt("##_clnt_sens_gain_", &_this->linearGain, 0, 21)) {
                    _this->clientInfo->setLinearGain(_this->linearGain, true);
                }
            }             
        } 
        
        if (connected) {            
            _this->frametimeCounter += ImGui::GetIO().DeltaTime;
            if (_this->frametimeCounter >= 0.2f) {
                _this->datarate = ((float)_this->client->bytes / (_this->frametimeCounter * 1024.0f * 1024.0f)) * 8;
                _this->frametimeCounter = 0;
                _this->client->bytes = 0;
            }

            ImGui::TextUnformatted("Статус:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Підключено (%.3f Мбіт/c)", _this->datarate);
        }
        else {
            ImGui::TextUnformatted("Статус:");
            ImGui::SameLine();
            ImGui::TextUnformatted("He підключено (--.--- Мбіт/с)");
        }        

    }

    bool connected() {
        return client && client->isOpen();
    }

    bool connectedInfo() {
        return clientInfo && clientInfo->isOpen();
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate = 0;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
    std::shared_ptr<server::TCPClient> client;
    std::shared_ptr<server::TCPClient> clientInfo;

    bool running = false;
    double freq;

    char ip[1024] = "localhost";
    int port = 1234;
    int srId = 0;
    int directSamplingId = 0;
    int ppm = 0;
    int gain = 0;
    bool biasTee = false;
    bool offsetTuning = false;
    bool rtlAGC = false;
    bool tunerAGC = false;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;

    float datarate = 0;
    float frametimeCounter = 0;
    float volLevel = 0.5;
    int gainMode = 1;
    int linearGain = 0;

    OptionList<int, int> directSamplingModes;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rtl_tcp_config.json");
    config.load(json({}));
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new TCPClientSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (TCPClientSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}