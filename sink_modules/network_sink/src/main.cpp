#include <utils/networking.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <config.h>
#include <gui/style.h>
#include <core.h>

#include <utils/flog.h>
#include <version.h>
#include <config.h>
#include <filesystem>
#include <dsp/types.h>
#include <signal_path/signal_path.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>
#include "dsp/compression/sample_stream_compressor.h"
#include "dsp/sink/handler_sink.h"
#include <zstd.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "network_sink",
    /* Description:     */ "Network sink module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

enum {
    SINK_MODE_TCP,
    SINK_MODE_UDP
};

const char* sinkModesTxt = "TCP\0UDP\0";

class NetworkSink : SinkManager::Sink {
public:
    NetworkSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;
        changed = true;
        // Load config
        config.acquire();
        if (!config.conf.contains(_streamName)) {
            config.conf[_streamName]["hostname"] = "localhost";
            config.conf[_streamName]["port"] = 7355;
            /// config.conf[_streamName]["protocol"] = SINK_MODE_TCP; // UDP
            config.conf[_streamName]["sampleRate"] = 48000.0;
            /// config.conf[_streamName]["stereo"] = false;
            config.conf[_streamName]["listening"] = false;
        }
        std::string host = config.conf[_streamName]["hostname"];
        strcpy(hostname, host.c_str());
        port = config.conf[_streamName]["port"];
        modeId = SINK_MODE_TCP; // config.conf[_streamName]["protocol"];
        sampleRate = config.conf[_streamName]["sampleRate"];
        stereo = false; // true; // config.conf[_streamName]["stereo"];
        bool startNow = false; // config.conf[_streamName]["listening"];
        config.release(true);

        netBuf = new int16_t[STREAM_BUFFER_SIZE];

        packer.init(_stream->sinkOut, 1024);
        if (stereo) {
            stereoSink.init(&packer.out, stereoHandler, this);
        } else {
            s2m.init(&packer.out);
            monoSink.init(&s2m.out, monoHandler, this);
        }

        // Create a list of sample rates
        /*
        for (int sr = 12000; sr < 200000; sr += 12000) {
            sampleRates.push_back(sr);
        }
        for (int sr = 11025; sr < 192000; sr += 11025) {
            sampleRates.push_back(sr);
        }*/
        sampleRates.push_back(12000);
        sampleRates.push_back(24000);
        sampleRates.push_back(48000);
        sampleRates.push_back(60000);
        sampleRates.push_back(72000);
        sampleRates.push_back(192000);
        sampleRates.push_back(250000);

        // Sort sample rate list
        std::sort(sampleRates.begin(), sampleRates.end(), [](double a, double b) { return (a < b); });

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
            if (sr == 48000.0) { _48kId = id; }
            id++;
        }
        if (!found) {
            srId = _48kId;
            sampleRate = 48000.0;
        }
        flog::info("  2 sampleRate {0}", sampleRate);
        _stream->setSampleRate(sampleRate);

        priv_freq = 0; // gui::waterfall.getCenterFrequency();
        // packer.setSampleCount(sampleRate / 60);

        // Start if needed
        // if (startNow) { startServer(); }
    }

    ~NetworkSink() {
        stopServer();
        delete[] netBuf;
    }

    void start() {
        if (running) {
            return;
        }
        changed= true;
        doStart();
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        changed= true;
        doStop();
        running = false;
    }
    
    // #define MAIN_GET_PROCESSING 7

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool listening = (listener && listener->isListening()) || (conn && conn->isOpen());
        double curr_freq = gui::waterfall.getCenterFrequency();
        if(curr_freq!=priv_freq) {
            changed = true;
        }
        priv_freq = curr_freq;

        if (listening) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_network_sink_host_", _streamName), hostname, 1023)) {
            config.acquire();
            config.conf[_streamName]["hostname"] = std::string(hostname);
            config.release(true);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_network_sink_port_", _streamName), &port, 0, 0)) {
            config.acquire();
            config.conf[_streamName]["port"] = port;
            config.release(true);
            changed = true;
        }

        ImGui::LeftLabel("Частота дискретизації");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_sink_sr_", _streamName), &srId, sampleRatesTxt.c_str())) {
            sampleRate = sampleRates[srId];
            _stream->setSampleRate(sampleRate);
            packer.setSampleCount(sampleRate / 60);
            config.acquire();
            config.conf[_streamName]["sampleRate"] = sampleRate;
            config.release(true);
            changed = true;
        }

        if (listening) { style::endDisabled(); }

        if (listening && ImGui::Button(CONCAT("Стоп##_network_sink_stop_", _streamName), ImVec2(menuWidth, 0))) {
            stopServer();
            config.acquire();
            config.conf[_streamName]["listening"] = false;
            config.release(true);
        }
        else if (!listening && ImGui::Button(CONCAT("Передача в мережу##_network_sink_stop_", _streamName), ImVec2(menuWidth, 0))) {
            startServer();
            config.acquire();
            config.conf[_streamName]["listening"] = true;
            config.release(true);
            changed = true;
        }

        ImGui::TextUnformatted("Статус:");
        ImGui::SameLine();
        if (!listening) {
            ImGui::TextUnformatted("Вхолосту");
        } else {
            int _work;
            core::modComManager.callInterface("Airspy", 0, NULL, &_work);
            if (_work) {
                ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Надсилання"); //Green
            }  else {
                //if (!_work) { // (conn && conn->isOpen()) || 
                ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "З'єднання");
            }
        }
    }

private:
    void doStart() {
        _stop = false; 

        // Start worker
        workerThread = std::thread(&NetworkSink::infoSinkWorker, this);

        packer.start();
        flog::warn("Starting");
        if (stereo) {
            stereoSink.start();
        }
        else {
            flog::warn("Starting");
            s2m.start();
            monoSink.start();
        }        
    }

    void doStop() {
        _stop = true; 
       if (workerThread.joinable()) {
            workerThread.join();
        }

        packer.stop();
        if (stereo) {
            stereoSink.stop();
        } else {
            s2m.stop();
            monoSink.stop();
        }    
    }

    void infoSinkWorker() {
        flog::info("  infoSinkWorker!!!");

        // NetworkSink* _this = (NetworkSink*)ctx;
        while (true) {            
            if(_stop) break;
            if (connInfo && connInfo->isOpen()) {
                if(changed==true)
                    flog::info("     infoSinkWorker '{0}'!", changed);
                pktmsg msg;            
                uint32_t _freq = gui::waterfall.getCenterFrequency();
                std::string nameVFO = _streamName; // gui::waterfall.selectedVFO; 
                if (gui::waterfall.vfos.find(nameVFO) != gui::waterfall.vfos.end()) {
                    _freq += gui::waterfall.vfos[nameVFO]->generalOffset;
                }        
                /// flog::info("     nameVFO '{0}', _freq {1} !", nameVFO, _freq);                
                msg.freq = _freq; // htons(_freq);  
                msg.sampleRate = sampleRate; 
                msg.playing = gui::mainWindow.isPlaying();
                msg.level = _stream->getVolume();
                msg.gainMode   = gui::mainWindow.getGainMode();
                msg.linearGain = gui::mainWindow.getLinearGain();

                // Write to network
                connInfo->write(sizeof(msg), (uint8_t*) &msg);
                // flog::info("->writeInfo freq {0}, _freq {1}, nameVFO {2}, msg.id = {3}, msg.sampleRate {4} ", msg.freq, _freq, nameVFO, msg.id, msg.sampleRate);                
                
                connInfo->read(sizeof(msg), (uint8_t*) &msg, true);
                // flog::info("connInfo->read! msg->playing {0}, msg.clntsending = {1}, msg.level = {2}, gui::mainWindow.isPlaying() {3}",  msg.playing, msg.clntsending, msg.level, gui::mainWindow.isPlaying());
                if(gui::mainWindow.isPlaying()!=msg.playing){
                    if(msg.playing==false){
                        gui::mainWindow.setPlayState(false);
                    } else {
                        gui::mainWindow.setPlayState(true);
                    }
                }                
                if(msg.level!=_stream->getVolume()){
                    // flog::info("msg.level {0}", msg.level);
                    _stream->setVolume(msg.level);
                }
                if(msg.gainMode==1){
                    // flog::info("msg.gainMode {0}", msg.gainMode);
                    if(msg.linearGain!=gui::mainWindow.getLinearGain()){
                        flog::info("msg.linearGain {0}, gui::mainWindow.getLinearGain() {1}",msg.linearGain, gui::mainWindow.getLinearGain());
                        gui::mainWindow.setLinearGain(msg.linearGain, true);
                    }
                }
                changed= false;
            }        
            usleep(1000000); //0.5 сек
        }

    }

    void startServer() {
        if (modeId == SINK_MODE_TCP) {
            std::string host = "0.0.0.0";            
            std::string tmp_host = std::string(hostname);
            if(tmp_host=="localhost" || tmp_host=="192.168.0.1" || tmp_host=="127.0.0.1"){
                host = tmp_host;
            }
            flog::info("startServer!! {0}:{1}", host, port);
            try{
                listener = net::listen(host, port);
                if (listener) {
                    listener->acceptAsync(clientHandler, this); // this
                    flog::info("Ready, listening on {0}:{1}", host, port);
                    // while(1) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
                }   
            
                listenerInfo = net::listen(host, port+2);
                if (listenerInfo) {
                    listenerInfo->acceptAsync(clientInfoHandler, this); // this
                    flog::info("Ready, listening on {0}:{1}", host, port+2);
                    // while(1) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
                }
            }
            catch (std::exception e) {
                flog::error("Could not connect to spyserver {0}", e.what());
            }

        }
        else {
            conn = net::openUDP("0.0.0.0", port, hostname, port, false);
        }
    }

    void stopServer() {
        if (conn) { conn->close(); }
        if (connInfo) { connInfo->close(); }
        if (listener) { listener->close(); }
        if (listenerInfo) { listenerInfo->close(); }

    }

    static void monoHandler(float* samples, int count, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;
        std::lock_guard lck(_this->connMtx);

        if (!_this->conn || !_this->conn->isOpen()) { return; }
                
        int _size = count * sizeof(int16_t); 
        // flog::info("   Freq write!!! count * sizeof(int16_t) {0}, count {1}", _size, count);
        volk_32f_s32f_convert_16i(_this->netBuf, (float*)samples, 32768.0f, count);
        _size = _this->conn->write(count* sizeof(int16_t), (uint8_t*) _this->netBuf);
    }

    static void stereoHandler(dsp::stereo_t* samples, int count, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;
        std::lock_guard lck(_this->connMtx);
        if (!_this->conn || !_this->conn->isOpen()) { return; }

        volk_32f_s32f_convert_16i(_this->netBuf, (float*)samples, 32767.0f, count * 2);
        _this->conn->write(count * 2 * sizeof(int16_t), (uint8_t*)_this->netBuf);
        // flog::warn("stereoHandler count {0}}", count);
    }

    static void clientHandler(net::Conn client, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;

        {
            std::lock_guard lck(_this->connMtx);
            _this->conn = std::move(client);
        }

        if (_this->conn) {
            _this->conn->waitForEnd();
            _this->conn->close();
        }
        else {
        }

        _this->listener->acceptAsync(clientHandler, _this);
    }

    static void clientInfoHandler(net::Conn client, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;

        {
            std::lock_guard lck(_this->connMtx);
            _this->connInfo = std::move(client);
        }

        if (_this->connInfo) {
            _this->changed = true;
            flog::info("rcv connInfo changed {0}", _this->changed); 

            _this->connInfo->waitForEnd();
            _this->connInfo->close();
        }
        else {
        }
        _this->listenerInfo->acceptAsync(clientInfoHandler, _this);
    }

    SinkManager::Stream* _stream;
    dsp::buffer::Packer<dsp::stereo_t> packer;
    dsp::convert::StereoToMono s2m;
    dsp::sink::Handler<float> monoSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;

    std::thread workerThread;

    std::string _streamName;

    int srId = 0;
    bool running = false;

    char hostname[1024];
    int port = 4242;

    int modeId = SINK_MODE_TCP;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 48000;
    
    
    bool stereo = true;

    int16_t* netBuf;

    net::Listener listener;
    net::Listener listenerInfo;

    net::Conn conn;
    net::Conn connInfo;
    std::mutex connMtx;
    // std::mutex connMtxInfo;        
    double priv_freq = 0;
    bool changed = true;
    bool _stop = false;

    struct pktmsg
    {
        uint8_t id;
        uint32_t freq;
        uint32_t sampleRate;
        bool clntsending;
        bool playing;
        float level; 
        int gainMode;
        int linearGain;        
    };

};

class NetworkSinkModule : public ModuleManager::Instance {
public:
    NetworkSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;
        sigpath::sinkManager.registerSinkProvider("Мережа", provider);
    }

    ~NetworkSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Мережа");
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
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new NetworkSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/network_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    NetworkSinkModule* instance = new NetworkSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (NetworkSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}