#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <utils/flog.h>
#include <RtAudio.h>
#include <config.h>
#include <core.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Audio sink module for SDR++",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1};

ConfigManager config;

// Функция для корректировки частоты дискретизации в конфигурации
void adjust_sample_rates(json& config_json) {
    for (auto& [channel, channel_config] : config_json.items()) {
        if (channel_config.contains("devices")) {
            for (auto& [device, sample_rate] : channel_config["devices"].items()) {
                if (sample_rate.get<unsigned int>() > 16000) {
                    sample_rate = 8000;
                }
            }
        }
    }
}

class AudioSink : SinkManager::Sink
{
public:
    AudioSink(SinkManager::Stream *stream, std::string streamName)
    {
        _stream = stream;
        _streamName = streamName;
        s2m.init(_stream->sinkOut);
        monoPacker.init(&s2m.out, 512);
        stereoPacker.init(_stream->sinkOut, 512);

#if RTAUDIO_VERSION_MAJOR >= 6
        audio.setErrorCallback(&errorCallback);
#endif

        bool created = false;
        std::string device = "";
        config.acquire();
        if (!config.conf.contains(_streamName))
        {
            created = true;
            config.conf[_streamName]["device"] = "";
            config.conf[_streamName]["devices"] = json({});
        }

        // Применяем логику изменения частоты дискретизации
        if (config.conf.contains(_streamName) && config.conf[_streamName].contains("devices")) {
            for (auto& [dev_name, sample_rate] : config.conf[_streamName]["devices"].items()) {
                if (sample_rate.get<unsigned int>() > 16000) {
                    sample_rate = 8000;
                }
            }
        }

        device = config.conf[_streamName]["device"];
        config.release(created);

        RtAudio::DeviceInfo info;
#if RTAUDIO_VERSION_MAJOR >= 6
        for (int i : audio.getDeviceIds())
        {
#else
        int count = audio.getDeviceCount();
        for (int i = 0; i < count; i++)
        {
#endif
            try
            {
                info = audio.getDeviceInfo(i);
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
                if (!info.probed)
                {
                    continue;
                }
#endif
                if (info.outputChannels == 0)
                {
                    continue;
                }
                if (info.isDefaultOutput)
                {
                    defaultDevId = devList.size();
                }
                devList.push_back(info);
                deviceIds.push_back(i);
                txtDevList += info.name;
                txtDevList += '\0';
            }
            catch (const std::exception &e)
            {
                flog::error("AudioSinkModule Error getting audio device ({}) info: {}", i, e.what());
            }
        }
        selectByName(device);
    }

    ~AudioSink()
    {
        stop();
    }

    void start()
    {
        if (running.load())
        {
            return;
        }
        running.store(doStart());
    }

    void stop()
    {
        if (!running.load())
        {
            return;
        }
        doStop();
        running.store(false);
    }

    void selectFirst()
    {
        selectById(defaultDevId);
    }

    void selectByName(std::string name)
    {
        for (int i = 0; i < devList.size(); i++)
        {
            if (devList[i].name == name)
            {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id)
    {
        flog::info("selectById = {0}", id);
        devId = id;
        bool created = false;
        config.acquire();
        if (!config.conf[_streamName]["devices"].contains(devList[id].name))
        {
            created = true;
            config.conf[_streamName]["devices"][devList[id].name] = devList[id].preferredSampleRate;
        }

        // Применяем логику изменения частоты дискретизации
        unsigned int current_rate = config.conf[_streamName]["devices"][devList[id].name];
        if (current_rate > 16000) {
            current_rate = 8000;
            config.conf[_streamName]["devices"][devList[id].name] = current_rate;
        }
        sampleRate = current_rate;
        
        config.release(created);

        sampleRates = devList[id].sampleRates;
        sampleRatesTxt = "";
        char buf[256];
        bool found = false;
        unsigned int defaultId = 0;
        unsigned int defaultSr = devList[id].preferredSampleRate;
        for (int i = 0; i < sampleRates.size(); i++)
        {
            if (sampleRates[i] == sampleRate)
            {
                found = true;
                srId = i;
            }
            if (sampleRates[i] == defaultSr)
            {
                defaultId = i;
            }
            sprintf(buf, "%d", sampleRates[i]);
            sampleRatesTxt += buf;
            sampleRatesTxt += '\0';
        }
        if (!found)
        {
            sampleRate = defaultSr;
            if (sampleRate > 16000) {
                sampleRate = 8000;
            }
            
            // Ищем 8000 или 16000 в списке доступных
            bool found_alternative = false;
            for(int i = 0; i < sampleRates.size(); i++) {
                if (sampleRates[i] == 8000 || sampleRates[i] == 16000) {
                    srId = i;
                    sampleRate = sampleRates[i];
                    found_alternative = true;
                    break;
                }
            }
            if (!found_alternative) {
                srId = defaultId;
                sampleRate = defaultSr;
            }
        }
        flog::info("selectById. srId = {0}", srId);
        _stream->setSampleRate(sampleRate);

        if (running.load())
        {
            doStop();
        }
        if (running.load())
        {
            doStart();
        }
    }

    void menuHandler()
    {
        if (gui::mainWindow.getStopMenuUI())
        {
            return;
        }
        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("##_audio_sink_dev_" + _streamName).c_str(), &devId, txtDevList.c_str()))
        {
            selectById(devId);
            config.acquire();
            config.conf[_streamName]["device"] = devList[devId].name;
            config.release(true);
        }

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("##_audio_sink_sr_" + _streamName).c_str(), &srId, sampleRatesTxt.c_str()))
        {
            sampleRate = sampleRates[srId];
            if (sampleRate > 16000) {
                sampleRate = 8000;
            }
            _stream->setSampleRate(sampleRate);
            if (running.load())
            {
                doStop();
                doStart();
            }
            config.acquire();
            config.conf[_streamName]["devices"][devList[devId].name] = sampleRate;
            config.release(true);
        }
    }

#if RTAUDIO_VERSION_MAJOR >= 6
    static void errorCallback(RtAudioErrorType type, const std::string &errorText)
    {
        switch (type)
        {
        case RtAudioErrorType::RTAUDIO_NO_ERROR:
            return;
        case RtAudioErrorType::RTAUDIO_WARNING:
        case RtAudioErrorType::RTAUDIO_NO_DEVICES_FOUND:
        case RtAudioErrorType::RTAUDIO_DEVICE_DISCONNECT:
            flog::warn("AudioSinkModule Warning: {} ({})", errorText, (int)type);
            break;
        default:
            throw std::runtime_error(errorText);
        }
    }
#endif

private:
    bool doStart()
    {
        // std::lock_guard<std::mutex> lck(audioMtx);
        RtAudio::StreamParameters parameters;
        parameters.deviceId = deviceIds[devId];
        parameters.nChannels = 2;
        // unsigned int bufferFrames = sampleRate / 60;   // ~16.7 ms при 48 kHz
        // Цель: 40–80 мс. Например, ~64 мс при 48 kHz:
        unsigned int bufferFrames = std::max(1024u, sampleRate / 15); // ≈ 66.6 ms @ 48k        
        RtAudio::StreamOptions opts;
        // opts.flags = RTAUDIO_MINIMIZE_LATENCY;
        opts.flags = 0;
        opts.numberOfBuffers = 4;
        opts.streamName = _streamName;

        try
        {
            audio.openStream(&parameters, NULL, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &callback, this, &opts);
            stereoPacker.setSampleCount(bufferFrames);
            audio.startStream();
            stereoPacker.start();
        }
        catch (const std::exception &e)
        {
            flog::error("Could not open audio device {0}", e.what());
            return false;
        }

        flog::info("RtAudio stream open");
        return true;
    }

    void doStop()
    {
        // std::lock_guard<std::mutex> lck(audioMtx);
        s2m.stop();
        monoPacker.stop();
        stereoPacker.stop();
        monoPacker.out.stopReader();
        stereoPacker.out.stopReader();
        audio.stopStream();
        audio.closeStream();
        monoPacker.out.clearReadStop();
        stereoPacker.out.clearReadStop();
    }

    static int callback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames, double streamTime, RtAudioStreamStatus status, void *userData)
    {
        AudioSink *_this = (AudioSink *)userData;

        // std::lock_guard<std::mutex> lck(_this->audioMtx);
        int count = _this->stereoPacker.out.read();
        if (count < 0)
        {
            return 0;
        }

        memcpy(outputBuffer, _this->stereoPacker.out.readBuf, nBufferFrames * sizeof(dsp::stereo_t));
        _this->stereoPacker.out.flush();
        return 0;
    }

    SinkManager::Stream *_stream;
    dsp::convert::StereoToMono s2m;
    dsp::buffer::Packer<float> monoPacker;
    dsp::buffer::Packer<dsp::stereo_t> stereoPacker;

    std::string _streamName;

    int srId = 0;
    int devCount;
    int devId = 0;
    // bool running = false;
    std::atomic<bool> running = {false};

    unsigned int defaultDevId = 0;

    std::vector<RtAudio::DeviceInfo> devList;
    std::vector<unsigned int> deviceIds;
    std::string txtDevList;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 8000;
    // std::mutex audioMtx;
    RtAudio audio;
};

class AudioSinkModule : public ModuleManager::Instance
{
public:
    AudioSinkModule(std::string name)
    {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~AudioSinkModule()
    {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Audio");
    }

    void postInit() {}

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
    static SinkManager::Sink *create_sink(SinkManager::Stream *stream, std::string streamName, void *ctx)
    {
        return (SinkManager::Sink *)(new AudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_()
{
    json def = json({});
    config.setPath(core::args["root"].s() + "/audio_sink_config.json");
    config.load(def);

    // Применяем логику изменения частоты дискретизации ко всей конфигурации
    config.acquire();
    adjust_sample_rates(config.conf);
    config.release(true); // Сохраняем изменения

    config.enableAutoSave();
}

MOD_EXPORT void *_CREATE_INSTANCE_(std::string name)
{
    AudioSinkModule *instance = new AudioSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance)
{
    delete (AudioSinkModule *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
}