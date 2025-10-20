#include <imgui.h>
#include <module.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/convert/stereo_to_mono.h>
#include <thread>
#include <ctime>
#include <gui/gui.h>
#include <filesystem>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <recorder_interface.h>
#include <core.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include "../../decoder_modules/radio/src/radio_interface.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <gui/menus/source.h>

#include <curl/curl.h>
#include <utils/freq_formatting.h>
#include <utils/networking.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <dsp/sink/handler_sink.h>
#include <dsp/demod/pureIQ.h>
// #include <dsp/sink/handler.h>

#include <dsp/filter/fir.h>
#include <dsp/filter/decimating_fir.h>
#include <dsp/taps/tap.h>
#include <dsp/types.h>

// For pre-recording buffer
#include <deque>
#include <vector>

namespace fs = std::filesystem;
// using namespace std::filesystem;

#define CONCAT(a, b) ((std::string(a) + b).c_str())
#define ID_SEANCE true

SDRPP_MOD_INFO{
    /* Name:            */ "recorder",
    /* Description:     */ "Recorder module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 3, 7,
    /* Max instances    */ -1};

enum SamplingRate
{
    SAMP_TYPE_8k,
    SAMP_TYPE_11k025,
    SAMP_TYPE_16k,
    SAMP_TYPE_44к1,
    SAMP_TYPE_48k
};
ConfigManager config;

std::map<int, const char *> radioModeToStringEngl = {
    {RADIO_IFACE_MODE_NFM, "NFM"},
    {RADIO_IFACE_MODE_WFM, "WFM"},
    {RADIO_IFACE_MODE_AM, "AM"},
    {RADIO_IFACE_MODE_DSB, "DSB"},
    {RADIO_IFACE_MODE_USB, "USB"},
    {RADIO_IFACE_MODE_CW, "CW"},
    {RADIO_IFACE_MODE_LSB, "LSB"},
    {RADIO_IFACE_MODE_RAW, "RAW"}
    //    {RADIO_IFACE_MODE_IQ, "pureIQ"},
};
// "ЧМ\0ЧМ-Ш\0AM\0ПБС\0НБС\0НС\0ВБС\0CMO\0";
std::map<int, const char *> radioModeToString = {
    {RADIO_IFACE_MODE_NFM, "ЧМ"},
    {RADIO_IFACE_MODE_WFM, "ЧМ-Ш"},
    {RADIO_IFACE_MODE_AM, "AM"},
    {RADIO_IFACE_MODE_DSB, "ПБС"},
    {RADIO_IFACE_MODE_USB, "ВБС"},
    {RADIO_IFACE_MODE_CW, "HC"},
    {RADIO_IFACE_MODE_LSB, "НБС"},
    {RADIO_IFACE_MODE_RAW, "CMO"}
    //    {RADIO_IFACE_MODE_IQ, "IQ"}
};

#define diffSamplingRate 8000

enum class AkfState
{
    IDLE,             // АКФ неактивен
    RECORDING,        // Идет короткая запись для анализа
    ANALYSIS_PENDING, // Запись окончена, ждем результат от анализатора
    SIGNAL_DETECTED,  // Обнаружен сигнал, основная запись продолжается
    NOISE_DETECTED    // Обнаружен шум/таймаут, основная запись будет остановлена
};

// Вспомогательная функция для преобразования AkfState в строку
inline const char *akfStateToString(AkfState state)
{
    switch (state)
    {
    case AkfState::IDLE:
        return "IDLE";
    case AkfState::RECORDING:
        return "RECORDING";
    case AkfState::ANALYSIS_PENDING:
        return "ANALYSIS_PENDING";
    case AkfState::SIGNAL_DETECTED:
        return "SIGNAL_DETECTED";
    case AkfState::NOISE_DETECTED:
        return "NOISE_DETECTED";
    default:
        return "UNKNOWN";
    }
}

inline bool initShutdownPipe(int fds[2])
{
#if defined(__linux__)
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0)
        return true;
    // fallback (маловероятно)
#endif
    if (pipe(fds) < 0)
        return false;

    // NONBLOCK
    int f0 = fcntl(fds[0], F_GETFL, 0);
    if (f0 != -1)
        fcntl(fds[0], F_SETFL, f0 | O_NONBLOCK);
    int f1 = fcntl(fds[1], F_GETFL, 0);
    if (f1 != -1)
        fcntl(fds[1], F_SETFL, f1 | O_NONBLOCK);

    // CLOEXEC
    int c0 = fcntl(fds[0], F_GETFD, 0);
    if (c0 != -1)
        fcntl(fds[0], F_SETFD, c0 | FD_CLOEXEC);
    int c1 = fcntl(fds[1], F_GETFD, 0);
    if (c1 != -1)
        fcntl(fds[1], F_SETFD, c1 | FD_CLOEXEC);

    return true;
}

inline void wakeShutdownPipe(int write_fd)
{
    if (write_fd < 0)
        return;
    char x = 'x';
    ssize_t r = write(write_fd, &x, 1);
    if (r < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE)
            return;
        flog::warn("shutdown pipe write failed: %s", strerror(errno));
    }
}

inline void drainShutdownPipe(int read_fd)
{
    if (read_fd < 0)
        return;
    char buf[64];
    // read до опустошения, неблокирующий => безопасно
    while (read(read_fd, buf, sizeof(buf)) > 0)
    {
    }
}

inline void closeShutdownPipe(int fds[2])
{
    if (fds[0] != -1)
    {
        close(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] != -1)
    {
        close(fds[1]);
        fds[1] = -1;
    }
}

// Lock-free кольцевой буфер для одного производителя и одного потребителя (SPSC)
class LockFreeRingBuffer
{
public:
    // Конструктор: выделяет память под буфер
    LockFreeRingBuffer(size_t size) : _size(size), _head(0), _tail(0)
    {
        _buffer = new std::atomic<float>[size];
        for (size_t i = 0; i < size; ++i)
        {
            _buffer[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    // Деструктор: освобождает память
    ~LockFreeRingBuffer()
    {
        delete[] _buffer;
    }

    // Некопируемый
    LockFreeRingBuffer(const LockFreeRingBuffer &) = delete;
    LockFreeRingBuffer &operator=(const LockFreeRingBuffer &) = delete;

    // Запись в буфер (для аудиопотока)
    void push(float value)
    {
        const auto current_tail = _tail.load(std::memory_order_relaxed);
        _buffer[current_tail].store(value, std::memory_order_relaxed); // Записываем в любом случае

        auto next_tail = current_tail + 1;
        if (next_tail == _size)
        {
            next_tail = 0;
        }
        _tail.store(next_tail, std::memory_order_release);
    }

    // Чтение из буфера (для потока start())
    bool pop(float &value)
    {
        const auto current_head = _head.load(std::memory_order_relaxed);
        if (current_head == _tail.load(std::memory_order_acquire))
        {
            return false; // Буфер пуст
        }
        value = _buffer[current_head].load(std::memory_order_relaxed);
        auto next_head = current_head + 1;
        if (next_head == _size)
        {
            next_head = 0;
        }
        _head.store(next_head, std::memory_order_release);
        return true;
    }

private:
    const size_t _size;
    std::atomic<float> *_buffer;
    std::atomic<size_t> _head;
    std::atomic<size_t> _tail;
};

// =============================================================
// ГЛОБАЛЬНЫЕ ОБЪЕКТЫ И ПРЕДВАРИТЕЛЬНЫЕ ОБЪЯВЛЕНИЯ
// =============================================================
class RecorderModule; // Предварительное объявление

static std::vector<RecorderModule *> g_recorderInstances;
static std::mutex g_instancesMutex;

class RecorderModule : public ModuleManager::Instance
{
public:
    std::string name;
    bool workerNeedsStarting = false;
    std::thread workerInfoThread; // <-- Мы снова будем его использовать!

    static std::atomic<bool> g_stop_workers;

    RecorderModule(std::string name) : folderSelect("%ROOT%/recordings"), folderSelect_akf("%ROOT%/recordings"), monoPreBuffer(4000)
    {
        flog::info("start constructor RecorderModule");
        this->name = name;
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_recorderInstances.push_back(this);

        root = (std::string)core::args["root"];
        strcpy(nameTemplate, "$f_$h-$m-$s_$d-$M-$y");

        containers.define("WAV", wav::FORMAT_WAV);

        sampleTypes.define(wav::SAMP_TYPE_UINT8, "Uint8", wav::SAMP_TYPE_UINT8);
        sampleTypes.define(wav::SAMP_TYPE_INT16, "Int16", wav::SAMP_TYPE_INT16);
        sampleTypes.define(wav::SAMP_TYPE_INT32, "Int32", wav::SAMP_TYPE_INT32);
        sampleTypes.define(wav::SAMP_TYPE_FLOAT32, "Float32", wav::SAMP_TYPE_FLOAT32);
        sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16);

        samplingRates.define(SAMP_TYPE_8k, "8000", SAMP_TYPE_8k);
        samplingRates.define(SAMP_TYPE_11k025, "11025", SAMP_TYPE_11k025);
        samplingRates.define(SAMP_TYPE_16k, "16000", SAMP_TYPE_16k);
        samplingRates.define(SAMP_TYPE_44к1, "44100", SAMP_TYPE_44к1);
        samplingRates.define(SAMP_TYPE_48k, "48000", SAMP_TYPE_48k);
        samplingRateId = samplingRates.valueId(SAMP_TYPE_8k);

        containerId = containers.valueId(wav::FORMAT_WAV);

        maxRecDuration = 5;
        bool update_conf = false;
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
            Admin = core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            Admin = false;
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

        if (!maxRecDuration)
        {
            maxRecDuration = 1;
            update_conf = true;
            core::configManager.conf["maxRecDuration"] = maxRecDuration;
        }

        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            radioMode = 0;
        }

        try
        {
            flag_akf = core::configManager.conf["flagVA"];
        }
        catch (const std::exception &e)
        {
            flag_akf = false;
            update_conf = true;
            core::configManager.conf["flagVA"] = false;
        }

        akfUdpPort = 0;

        try
        {
            SIport = core::configManager.conf["SIport"];
        }
        catch (const std::exception &e)
        {
            SIport = 63100;
            update_conf = true;
            core::configManager.conf["SIport"] = 63100;
        }
        try
        {
            maxRecShortDur_sec = core::configManager.conf["maxRecShortDur_sec"];
        }
        catch (const std::exception &e)
        {
            maxRecShortDur_sec = 2;
            update_conf = true;
            core::configManager.conf["maxRecShortDur_sec"] = 3;
        }

        try
        {
            shortRecDirectory = core::configManager.conf["RecDirectoryShort"];
        }
        catch (const std::exception &e)
        {
            shortRecDirectory = "/var/lib/avr/cws/data/receiver/va_only";
            update_conf = true;
            core::configManager.conf["RecDirectoryShort"] = shortRecDirectory;
        }
        try
        {
            longRecDirectory = core::configManager.conf["RecDirectoryLong"];
        }
        catch (const std::exception &e)
        {
            longRecDirectory = "/var/lib/avr/cws/data/receiver/records";
            update_conf = true;
            core::configManager.conf["RecDirectoryLong"] = longRecDirectory;
        }

        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];

        if (update_conf)
            core::configManager.release(true);
        else
            core::configManager.release();

        update_conf = false;
        config.acquire();
        std::string wavPath = "%ROOT%/recordings";
        try
        {
            wavPath = core::configManager.conf["PathWav"];
        }
        catch (...)
        {
            wavPath = "%ROOT%/recordings";
            std::cout << "Error config.conf M1" << std::endl;
        }
        folderSelect.setPath(wavPath);
        folderSelect_akf.setPath(wavPath);

        if (config.conf[name].contains("mode"))
        {
            recMode = config.conf[name]["mode"];
        }
        else
        {
            recMode = 1;
        }
        if (recMode == RECORDER_MODE_AUDIO)
        {
            sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16);
        }
        else
        {
            sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_FLOAT32);
        }
        if (config.conf[name].contains("audioStream"))
        {
            selectedStreamName = config.conf[name]["audioStream"];
        }
        else
        {
            selectedStreamName = "";
        }
        strm_name = "";
        if (name == "Запис")
        {
            strm_name = "Канал приймання";
            NUM_INST = 0;
        }
        else
        {
            flog::info("[RADIO] shortRecDirectory {}", shortRecDirectory);
            NUM_INST = 0;
            strm_name = "C1";
            if (name == "Запис C1")
            {
                strm_name = "C1";
                NUM_INST = 0;
            }
            else if (name == "Запис C2")
            {
                strm_name = "C2";
                NUM_INST = 1;
            }
            else if (name == "Запис C3")
            {
                strm_name = "C3";
                NUM_INST = 2;
            }
            else if (name == "Запис C4")
            {
                strm_name = "C4";
                NUM_INST = 3;
            }
            else if (name == "Запис C5")
            {
                strm_name = "C5";
                NUM_INST = 4;
            }
            else if (name == "Запис C6")
            {
                strm_name = "C6";
                NUM_INST = 5;
            }
            else if (name == "Запис C7")
            {
                strm_name = "C7";
                NUM_INST = 6;
            }
            else if (name == "Запис C8")
            {
                strm_name = "C8";
                NUM_INST = 7;
            }
            else
            {
                NUM_INST = 0;
            }
            try
            {
                shortRecDirectory = renameVaRootDir(shortRecDirectory, NUM_INST);
            }
            catch (const std::exception &e)
            {
                flog::error("Invalid shortRecDirectory format: {} — {}", shortRecDirectory, e.what());
                akfState.store(AkfState::IDLE);
                initiateSuccessfulRecording(1);
                return;
            }
        }
        akfUdpPort = SIport + NUM_INST;
        flog::info("[RECORDER {0}] Map: NUM_INST={1}, shortRecDirectory={2}, akfUdpPort={3}", name.c_str(), NUM_INST, shortRecDirectory.c_str(), akfUdpPort);

        if (strm_name == "")
            strm_name = "Канал приймання";

        flog::warn("radio. 1. strm_name .{0}., selectedStreamName {1}, name.size() {2}", strm_name.c_str(), selectedStreamName, name.size());
        if (selectedStreamName != strm_name)
        {
            selectedStreamName = std::string(strm_name);
            config.conf[name]["audioStream"] = strm_name;
            update_conf = true;
        }
        if (config.conf[name].contains("audioVolume"))
        {
            audioVolume = config.conf[name]["audioVolume"];
        }
        else
        {
            audioVolume = 0.5;
        }
        if (config.conf[name].contains("stereo"))
        {
            stereo = config.conf[name]["stereo"];
        }
        stereo = false;
        if (config.conf[name].contains("ignoreSilence"))
        {
            ignoreSilence = config.conf[name]["ignoreSilence"];
        }
        else
        {
            ignoreSilence = false;
        }

        if (config.conf[name].contains("nameTemplate"))
        {
            std::string _nameTemplate = config.conf[name]["nameTemplate"];
            if (_nameTemplate.length() > sizeof(nameTemplate) - 1)
            {
                _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate) - 1);
            }
            strcpy(nameTemplate, _nameTemplate.c_str());
        }
        else
        {
            if (config.conf["Запис"].contains("nameTemplate"))
            {
                std::string _nameTemplate = config.conf["Запис"]["nameTemplate"];
                if (_nameTemplate.length() > sizeof(nameTemplate) - 1)
                {
                    _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate) - 1);
                }
                strcpy(nameTemplate, _nameTemplate.c_str());
            }
            config.conf[name]["nameTemplate"] = nameTemplate;
            config.conf[name]["audioVolume"] = audioVolume;
            config.conf[name]["container"] = "WAV";
            config.conf[name]["ignoreSilence"] = ignoreSilence;
            config.conf[name]["sampleType"] = 0;
            update_conf = true;
        }

        // Pre-recording buffer settings
        if (config.conf[name].contains("preRecord"))
        {
            preRecord = config.conf[name]["preRecord"];
        }
        else
        {
            preRecord = true; // Значение по умолчанию
            config.conf[name]["preRecord"] = preRecord;
            update_conf = true;
        }

        if (config.conf[name].contains("preRecordTime"))
        {
            preRecordTimeMs = config.conf[name]["preRecordTime"];
        }
        else
        {
            preRecordTimeMs = 500; // Значение по умолчанию
            config.conf[name]["preRecordTime"] = preRecordTimeMs;
            update_conf = true;
        }

        if (update_conf)
            config.release(true);
        else
            config.release();

        flog::info("staring 0 constructor RecorderModule");

        thisInstance = thisInstance + "-1";

        // --- Start of Corrected DSP Chain ---
        volume.init(NULL, audioVolume, false);
        stereoSplitter.init(&volume.out);
        stereoSplitter.bindStream(&meterStream);
        stereoSplitter.bindStream(&s2mStream);
        meter.init(&meterStream);
        s2m.init(&s2mStream);
        monoSplitter.init(&s2m.out);
        monoSplitter.bindStream(&monoSinkStream);
        monoSplitter.bindStream(&preRecordSinkStream);

        flog::info("staring 1 constructor RecorderModule");

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this); // Kept for other modes, but not used for mono recording
        monoSink.init(&monoSinkStream, monoHandler, this);
        preRecordSink.init(&preRecordSinkStream, preRecordMonoHandler, this);

        flog::info("staring 2 constructor RecorderModule");

        if (!initShutdownPipe(shutdownPipeFd))
        {
            flog::error("Failed to create shutdown pipe for '%s': %s",
                        name.c_str(), strerror(errno));
        }
        else
        {
            flog::info("Shutdown pipe created: rfd=%d, wfd=%d", shutdownPipeFd[0], shutdownPipeFd[1]);
        }
        processing = 0;
        gui::menu.registerEntry(name, menuHandler, this);

        flog::info("finish constructor RecorderModule");

        flog::warn(" RegisterInterface: {0}", name);
        core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler, this);
    }

    ~RecorderModule()
    {
        is_destructing.store(true);
        try
        {
            stop(true);
        }
        catch (...)
        {
            flog::error("DESTRUCTOR '{0}': stop(true) threw; continuing cleanup.", name);
        }

        if (workerInfoThread.joinable())
            workerInfoThread.join();
        if (analysisThread.joinable())
            analysisThread.join();

        closeShutdownPipe(shutdownPipeFd);

        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);

        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        meter.stop();
        if (writer_akf)
        {
            delete writer_akf;
            writer_akf = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(g_instancesMutex);
            g_recorderInstances.erase(
                std::remove(g_recorderInstances.begin(), g_recorderInstances.end(), this),
                g_recorderInstances.end());
        }
    }

    std::string genRawFileName(int mode, std::string name)
    {
        std::string templ = "$t_$f_$h-$m-$s_$d-$M-$y.raw";
        time_t now = time(0);
        tm *ltm = localtime(&now);
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end())
        {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        std::string type = (recMode == RECORDER_MODE_AUDIO) ? "audio" : "iq";

        char freqStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        const char *modeStr = (recMode == RECORDER_MODE_AUDIO) ? "Unknown" : "IQ";
        sprintf(freqStr, "%.0lfHz", freq);
        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);
        if (core::modComManager.getModuleName(name) == "radio")
        {
            int mode = gui::mainWindow.getselectedDemodID();
            if (mode >= 0)
            {
                modeStr = radioModeToString[mode];
            };
        }

        templ = std::regex_replace(templ, std::regex("\\$t"), type);
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    std::string genWavFileName(const double current, const int _mode)
    {
        std::string templ = "$y$M$d-$u-$f-$b-$n-$m.wav";

        time_t now = time(0);
        tm *ltm = localtime(&now);
        using namespace std::chrono;
        milliseconds ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());

        flog::info("radio. 2. genWavFileName: {0}, ms {1}, current {2}", templ, std::to_string(ms.count()), current);
        double band = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        if (recMode == RECORDER_MODE_BASEBAND)
        {
            if (_mode == 7)
            {
                baseband_band = gui::mainWindow.getCMO_BBand();
                band = baseband_band;
            }
        }
        if (_mode == RECORDER_MODE_PUREIQ)
        {
            baseband_band = band;
            flog::info("radio. RECORDER_MODE_BASEBAND. band {0}, baseband_band: {1}", band, baseband_band);
        }

        char freqStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        char bandStr[128];

        sprintf(freqStr, "%.0lf", current);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year - 100);
        sprintf(bandStr, "%d", int(band));
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);

        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$u"), std::to_string(ms.count()));
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), demodModeListFile[_mode]);
        templ = std::regex_replace(templ, std::regex("\\$b"), bandStr);
        templ = std::regex_replace(templ, std::regex("\\$n"), thisInstance);
        flog::info("radio. templ 3 = {0}", templ);
        return templ;
    }

    void postInit()
    {
        // Enumerate streams
        flog::info("start postInit RecorderModule");

        audioStreams.clear();
        auto names = sigpath::sinkManager.getStreamNames();
        for (const auto &name : names)
        {
            audioStreams.define(name, name, name);
        }

        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        flog::info("starting 2 postInit RecorderModule");

        if (this->name == "Запис")
        {
            core::configManager.acquire();
            if (core::configManager.conf["IsServer"] == true)
                isServer = true;
            else
                isServer = false;
            core::configManager.release();
            currSource = sourcemenu::getCurrSource();
            if (currSource == SOURCE_ARM)
                isARM = true;
            else
                isARM = false;
            isControl = false;
        }
        else
        {
            isARM = false;
            isServer = false;
            isControl = true;
        }

        selectStream(selectedStreamName);
        flog::info("[postInit] Checking conditions for workerInfoThread. isServer: {0}, isARM: {1}, isControl {2}", isServer, isARM, isControl);
        if (isServer || isARM || isControl)
        {
            workerInfoThread = std::thread(&RecorderModule::workerInfo, this);
            flog::info("[postInit] Instance '{0}' is marked for worker thread start.", name);
        }
        else
        {
            flog::warn("[postInit] workerInfoThread will NOT be started for instance '{0}' due to conditions.", name);
        }
        flog::info("finish postInit RecorderModule");
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

    static void workerInfo(void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        flog::info("[WorkerInfo] Thread started for instance '{0}'.", _this->name);

        while (!g_stop_workers.load() && !_this->is_destructing.load())
        {
            if (_this->restart_pending.load())
            {
                flog::info("[WorkerInfo] Executing restart for '{0}'...", _this->name);
                _this->restart_pending.store(false);
                _this->currWavFile.clear();
                _this->start();
            }

            if (core::g_isExiting)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            if (_this->is_destructing.load())
                break;

            if (!_this->isControl)
            {
                if (_this->isServer && _this->name == "Запис")
                {
                    if (gui::mainWindow.getUpdateMenuRcv3Record())
                    {
                        bool should_record_from_gui = gui::mainWindow.getServerRecording(0);
                        flog::info("[WorkerInfo SERVER] GUI Command: Set recording to {0}.", should_record_from_gui);

                        if (should_record_from_gui)
                        {
                            if (!_this->recording.load())
                            {
                                _this->selectStream("Канал приймання");
                                _this->currWavFile.clear();
                                _this->akfState.store(AkfState::IDLE);
                                gui::mainWindow.setServerRecordingStart(gui::mainWindow.getCurrServer());
                                _this->start();
                                gui::mainWindow.setServerRecordingStart(gui::mainWindow.getCurrServer());
                                _this->processing = 1;
                            }
                        }
                        else
                        {
                            if (_this->recording.load())
                            {
                                gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                                _this->stop();
                                gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                                _this->processing = 0;
                            }
                        }

                        gui::mainWindow.setUpdateMenuRcv3Record(false);
                        continue;
                    }
                }
            }

            AkfState currentState = _this->akfState.load();
            bool restart_needed = _this->_restart.load();

            if (currentState == AkfState::ANALYSIS_PENDING ||
                currentState == AkfState::NOISE_DETECTED ||
                currentState == AkfState::SIGNAL_DETECTED ||
                restart_needed)
            {
                currentState = _this->akfState.load();

                if (currentState == AkfState::ANALYSIS_PENDING)
                {
                    if (_this->writer_akf)
                    {
                        flog::info("[WorkerInfo] '{0}': ANALYSIS_PENDING -> moving AKF file once.", _this->name);
                        _this->stop_akf(true);
                    }
                }
                else if (currentState == AkfState::NOISE_DETECTED)
                {
                    flog::info("[WorkerInfo] '{0}': NOISE_DETECTED. Stopping main recording.", _this->name);
                    if (_this->recording.load())
                    {
                        _this->stop(false);
                        if (!_this->_restart.load())
                        {
                            gui::mainWindow.setRecording(_this->recording.load());
                            gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                        }
                    }
                    if (std::filesystem::exists(_this->curr_expandedPath_akf))
                        std::filesystem::remove(_this->curr_expandedPath_akf);

                    _this->akfState.store(AkfState::IDLE);
                }
                else if (currentState == AkfState::SIGNAL_DETECTED)
                {
                    flog::info("[WorkerInfo] '{0}': SIGNAL_DETECTED. Finalizing recording.", _this->name);
                    if (_this->recording.load())
                        _this->initiateSuccessfulRecording(_this->Signal);

                    _this->akfState.store(AkfState::IDLE);
                }

                if (_this->_restart.load())
                {
                    flog::info("[WorkerInfo] '{0}': Restarting due to max duration.", _this->name);
                    _this->_restart.store(false);
                    _this->restart();
                }
            }
        }

        flog::info("[WorkerInfo] Thread for '{0}' is exiting.", _this->name);
    }

    void start()
    {
        bool should_start_analysis_thread = false;
        if (name == "Запис")
            gui::mainWindow.setUpdateMenuRcv3Record(false);

        {
            flog::info("Starting start()");
            std::unique_lock<std::recursive_mutex> lck(recMtx);
            if (recording.load())
            {
                return;
            }
            flog::info("Starting start() 1");

            analysisResultSignal.store(ANALYSIS_PENDING);
            Signal = -1;
            initialized = false;
            rms_history.clear();
            insert_REC.store(false);

            core::configManager.acquire();
            try
            {
                radioMode = (int)core::configManager.conf["RadioMode"];
                maxRecDuration = core::configManager.conf["maxRecDuration"];
                wavPath = core::configManager.conf["PathWav"];
                saveInDir = core::configManager.conf["SaveInDirMalva"];
            }
            catch (const std::exception &e)
            {
                flog::warn("Could not read all config values, using defaults. Error: {0}", e.what());
                radioMode = 0;
                wavPath = "%ROOT%/recordings";
                saveInDir = false;
            }
            try
            {
                SIport = core::configManager.conf["SIport"];
            }
            catch (const std::exception &e)
            {
                SIport = 63100;
            }
            core::configManager.release();

            strm_name = "";
            if (name == "Запис")
            {
                strm_name = "Канал приймання";
                NUM_INST = 0;
            }
            else
            {
                NUM_INST = 0;
                strm_name = "C1";
                if (name == "Запис C1")
                {
                    strm_name = "C1";
                    NUM_INST = 0;
                }
                else if (name == "Запис C2")
                {
                    strm_name = "C2";
                    NUM_INST = 1;
                }
                else if (name == "Запис C3")
                {
                    strm_name = "C3";
                    NUM_INST = 2;
                }
                else if (name == "Запис C4")
                {
                    strm_name = "C4";
                    NUM_INST = 3;
                }
                else if (name == "Запис C5")
                {
                    strm_name = "C5";
                    NUM_INST = 4;
                }
                else if (name == "Запис C6")
                {
                    strm_name = "C6";
                    NUM_INST = 5;
                }
                else if (name == "Запис C7")
                {
                    strm_name = "C7";
                    NUM_INST = 6;
                }
                else if (name == "Запис C8")
                {
                    strm_name = "C8";
                    NUM_INST = 7;
                }
                else
                {
                    NUM_INST = 0;
                }
                try
                {
                    shortRecDirectory = renameVaRootDir(shortRecDirectory, NUM_INST);
                }
                catch (const std::exception &e)
                {
                    flog::error("Invalid shortRecDirectory format: {} — {}", shortRecDirectory, e.what());
                    NUM_INST = 0;
                }
            }
            akfUdpPort = SIport + NUM_INST;
            flog::info("[RECORDER START {0}] Map: NUM_INST={1}, shortRecDirectory={2}, akfUdpPort={3}", name.c_str(), NUM_INST, shortRecDirectory.c_str(), akfUdpPort);

            if (currWavFile.empty())
            {
                int _mode = gui::mainWindow.getselectedDemodID();
                std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "Канал приймання";
                flog::info("calc current freq...");
                current = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[vfoName]->generalOffset;
                flog::info("current freq ready: %.0f", current);
                currWavFile = (recMode == RECORDER_MODE_AUDIO) ? expandString(genWavFileName(current, _mode)) : genRawFileName(recMode, vfoName);
            }
            flog::info("Starting recording for file: {0}", currWavFile);

            if (recMode == RECORDER_MODE_AUDIO)
            {
                if (selectedStreamName.empty())
                    return;
                samplerate = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
            }
            else
            {
                samplerate = sigpath::iqFrontEnd.getSampleRate();
            }
            writer.setFormat(containers[containerId]);
            // Correctly set channels to 1 for mono audio recording.
            writer.setChannels((recMode == RECORDER_MODE_AUDIO) ? 1 : 2);
            writer.setSampleType(sampleTypes[sampleTypeId]);
            writer.setSamplerate(samplerate);

            std::string tmp_dir = wavPath + "/../tmp_recording";
            makeDir(wavPath.c_str());
            makeDir(tmp_dir.c_str());
            expandedPath = expandString(tmp_dir + "/" + currWavFile);
            curr_expandedPath = expandString(wavPath + "/" + currWavFile);

            flog::info("Opening main recording file: {0}", expandedPath);
            if (!writer.open(expandedPath))
            {
                flog::error("Failed to open main file for recording: {0}. Aborting start.", expandedPath);
                return;
            }

            flog::info("preRecord {0} && isControlledChannel({1}) = {2}  && preRecordUse {3}", preRecord, name, isControlledChannel(name), preRecordUse);
            if (preRecord && isControlledChannel(name) && preRecordUse)
            {
                flog::info("Writing pre-record buffer to file...");
                std::vector<float> tempBuf;
                tempBuf.reserve(96000); // Резервируем с запасом
                float sample;
                while (monoPreBuffer.pop(sample))
                {
                    tempBuf.push_back(sample);
                }

                if (!tempBuf.empty())
                {
                    writer.write(tempBuf.data(), tempBuf.size());
                    flog::info("Wrote {0} mono samples from pre-record buffer.", tempBuf.size());
                }
            }

            flog::info("[RECORDER {0}] Entering start(). Current state: {1}, recMode: {2}",
                       name.c_str(), akfStateToString(akfState.load()), recMode);

            const bool use_akf = wantAKF();

            if (use_akf)
            {
                if (!akfPreflightOk())
                {
                    AkfState old = akfState.load();
                    akfState.store(AkfState::IDLE);
                    flog::warn("[RECORDER {0}] [AKF] preflight failed → downgrade {1} -> {2}",
                               name.c_str(), akfStateToString(old), akfStateToString(akfState.load()));
                }
            }
            flog::info("Entering start(). Current state: {0}, recMode: {1}", akfStateToString(akfState.load()), recMode);
            if (akfState.load() == AkfState::RECORDING && recMode == RECORDER_MODE_AUDIO)
            {
                if (writer_akf)
                {
                    delete writer_akf;
                    writer_akf = nullptr;
                }
                writer_akf = new wav::Writer();

                writer_akf->setFormat(containers[containerId]);
                writer_akf->setChannels(1);
                writer_akf->setSampleType(sampleTypes[sampleTypeId]);
                writer_akf->setSamplerate(samplerate);

                std::string akf_final_dir = shortRecDirectory;
                std::string akf_tmp_dir = shortRecDirectory + "/../tmp_recording";

                try
                {
                    std::filesystem::create_directories(akf_final_dir);
                    std::filesystem::create_directories(akf_tmp_dir);
                }
                catch (const std::exception &e)
                {
                    flog::error("[RECORDER {0}] [AKF] create_directories failed: {1}",
                                name.c_str(), e.what());
                }

                expandedPath_akf = expandString(akf_tmp_dir + "/va_" + currWavFile);
                curr_expandedPath_akf = expandString(akf_final_dir + "/" + currWavFile);

                flog::info("[RECORDER {0}] [AKF] shortRecDirectory={1}",
                           name.c_str(), shortRecDirectory.c_str());
                flog::info("[RECORDER {0}] [AKF] AKF temp  file: {1}",
                           name.c_str(), expandedPath_akf.c_str());
                flog::info("[RECORDER {0}] [AKF] AKF final file: {1}",
                           name.c_str(), curr_expandedPath_akf.c_str());

                if (writer_akf->open(expandedPath_akf))
                {
                    should_start_analysis_thread = true;
                }
                else
                {
                    flog::error("[RECORDER {0}] [AKF] Failed to open temp AKF file: {1}",
                                name.c_str(), expandedPath_akf.c_str());
                    akfState.store(AkfState::IDLE);
                    initiateSuccessfulRecording(1);
                }
            }
            else
            {
                flog::info("[RECORDER {0}] AKF is DISABLED for this session (state={1}, flag_akf={2}).",
                           name.c_str(), akfStateToString(akfState.load()), flag_akf);
                initiateSuccessfulRecording(1);
            }

            if (recMode == RECORDER_MODE_AUDIO)
            {
                monoSink.start();
            }
            else if (recMode == RECORDER_MODE_PUREIQ)
            {
                stereoSink.start();
                dummySink.init(&s2mStream, dummyHandler, this);
                dummySink.start();
            }
            else
            {
                basebandStream = new dsp::stream<dsp::complex_t>();
                basebandSink.setInput(basebandStream);
                basebandSink.start();
                sigpath::iqFrontEnd.bindIQStream(basebandStream);
            }

            recording.store(true);
            _restart.store(false);
            gui::mainWindow.setRecording(recording.load());
            flog::info("Starting 1 ...");
        }
        flog::info("Starting 2 ...");

        if (should_start_analysis_thread)
        {
            flog::info("Starting AKF analysis thread...");
            runAnalysisTask();
        }
        flog::info("Starting 3 ...");
    }

    void stop_akf(bool flag_rename = true)
    {
        flog::info("[RECORDER {0}] [STOP_AKF] 1. Called with flag_rename: {1}",
                   name.c_str(), flag_rename);

        std::lock_guard<std::recursive_mutex> lck(recMtx);

        if (!writer_akf)
        {
            flog::error("[RECORDER {0}] [STOP_AKF] 2. writer_akf is null. Aborting.",
                        name.c_str());
            return;
        }

        if (writer_akf->isOpen())
        {
            flog::info("[RECORDER {0}] [STOP_AKF] 3. Closing AKF writer for file: {1}",
                       name.c_str(), expandedPath_akf);
            writer_akf->close();
        }

        if (flag_rename)
        {
            try
            {
                flog::info("[RECORDER {0}] [STOP_AKF] 4a. Moving file for analysis from {1} to {2}",
                           name.c_str(), expandedPath_akf, curr_expandedPath_akf);
                std::filesystem::rename(expandedPath_akf, curr_expandedPath_akf);
            }
            catch (const std::filesystem::filesystem_error &e)
            {
                flog::error("[RECORDER {0}] [STOP_AKF] 5a. Error moving AKF file: {1}",
                            name.c_str(), e.what());
            }
        }
        else
        {
            flog::info("[RECORDER {0}] [STOP_AKF] 4b. Deleting temporary AKF file: {1}",
                       name.c_str(), expandedPath_akf);
            std::filesystem::remove(expandedPath_akf);
        }

        delete writer_akf;
        writer_akf = nullptr;

        flog::info("[RECORDER {0}] [STOP_AKF] 6. Cleanup complete.", name.c_str());
    }

    void stop(bool rename_and_save = true)
    {
        // ИСПОЛЬЗУЕМ БЛОКИРОВКУ НА ВСЮ ФУНКЦИЮ
        // std::lock_guard<std::mutex> lock(stopMtx);
        flog::info("[RECORDER STOP]");
        preRecordSink.stop();

        if (isStopping.exchange(true))
        {
            flog::warn("[RECORDER STOP] Already stopping '{0}'.", name);
            // Даже если выходим, preRecordSink уже остановлен, что безопасно.
            return;
        }

        // Теперь флаг 'recording' - наша единственная проверка, нужна ли очистка.
        // Флаг 'isStopping' больше не нужен для предотвращения двойного входа.
        if (!recording.load())
        {
            flog::warn("[RECORDER STOP] Already not recording '{0}'.", name);
            return;
        }

        flog::info("[RECORDER STOP] Cleanup sequence started for '{0}'...", name);

        // Сигнализируем потоку анализа, что нужно остановиться
        pleaseStopAnalysis.store(true);
        restart_pending.store(false);
        wakeShutdownPipe(shutdownPipeFd[1]);

        // Ждем завершения потока анализа
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lk(analysisThreadMtx);
            if (analysisThread.joinable())
                to_join = std::move(analysisThread);
        }
        if (to_join.joinable())
            to_join.join();

        // Эта проверка теперь избыточна, но оставим для безопасности.
        // Главное, что `recording.exchange(false)` теперь будет вызван только один раз.
        if (recording.exchange(false))
        {
            flog::info("[RECORDER] stop(): Stopping streams and closing writer...");

            if (recMode == RECORDER_MODE_AUDIO)
            {
                monoSink.stop();
            }
            else if (recMode == RECORDER_MODE_PUREIQ)
            {
                stereoSink.stop();
                dummySink.stop();
            }
            else
            {
                if (basebandStream)
                {
                    sigpath::iqFrontEnd.unbindIQStream(basebandStream);
                    basebandSink.stop();
                    delete basebandStream;
                    basebandStream = nullptr;
                }
            }

            writer.close();

            if (!rename_and_save)
            {
                try
                {
                    std::filesystem::remove(expandedPath);
                }
                catch (const std::filesystem::filesystem_error &e)
                {
                    flog::error("[RECORDER] remove temp failed: {0}", e.what());
                }
            }
            else
            {
                uint64_t seconds = writer.getSamplesWritten() / samplerate;
                if (seconds >= 2)
                {
                    try
                    {
                        int result = parseSixthBlock(currWavFile);
                        if (result == 2 && !saveInDir)
                        {
                            // ...
                        }
                        std::filesystem::rename(expandedPath, curr_expandedPath);
                    }
                    catch (const std::filesystem::filesystem_error &e)
                    {
                        flog::error("[RECORDER] rename/move failed: {0}", e.what());
                    }
                }
                else
                {
                    try
                    {
                        std::filesystem::remove(expandedPath);
                    }
                    catch (const std::filesystem::filesystem_error &e)
                    {
                        flog::error("[RECORDER] remove short file failed: {0}", e.what());
                    }
                }
            }

            if (status_direction && this_record)
            {
                curlPOST_end_new();
                status_direction = false;
            }
        }
        else
        {
            flog::info("[RECORDER] stop(): Already not recording. Skipping DSP/file close.");
        }

        // Финальная очистка состояния
        this_record = false;
        currWavFile.clear();
        akfState.store(AkfState::IDLE);
        analysisResultSignal.store(ANALYSIS_NONE);

        gui::mainWindow.setRecording(false);
        flog::info("[RECORDER] Stop sequence for '{0}' finished.", name);
    }

    void stop2(bool rename_and_save = true)
    {
        if (isStopping.exchange(true))
        {
            flog::warn("[RECORDER STOP] Already stopping '{0}'.", name);
            return;
        }

        flog::info("[RECORDER STOP] Cleanup sequence started for '{0}'...", name);

        pleaseStopAnalysis.store(true);
        restart_pending.store(false);
        wakeShutdownPipe(shutdownPipeFd[1]);

        std::thread to_join;
        {
            std::lock_guard<std::mutex> lk(analysisThreadMtx);
            if (analysisThread.joinable())
                to_join = std::move(analysisThread);
        }
        if (to_join.joinable())
            to_join.join();

        if (recording.load())
        {
            flog::info("[RECORDER] stop(): Stopping streams and closing writer...");

            if (recMode == RECORDER_MODE_AUDIO)
            {
                monoSink.stop();
            }
            else if (recMode == RECORDER_MODE_PUREIQ)
            {
                stereoSink.stop();
                dummySink.stop();
            }
            else
            {
                if (basebandStream)
                {
                    sigpath::iqFrontEnd.unbindIQStream(basebandStream);
                    basebandSink.stop();
                    delete basebandStream;
                    basebandStream = nullptr;
                }
            }

            writer.close();

            if (!rename_and_save)
            {
                try
                {
                    std::filesystem::remove(expandedPath);
                }
                catch (const std::filesystem::filesystem_error &e)
                {
                    flog::error("[RECORDER] remove temp failed: {0}", e.what());
                }
            }
            else
            {
                uint64_t seconds = writer.getSamplesWritten() / samplerate;
                if (seconds >= 2)
                {
                    try
                    {
                        int result = parseSixthBlock(currWavFile);
                        if (result == 2 && !saveInDir)
                        {
                            // ...
                        }
                        std::filesystem::rename(expandedPath, curr_expandedPath);
                    }
                    catch (const std::filesystem::filesystem_error &e)
                    {
                        flog::error("[RECORDER] rename/move failed: {0}", e.what());
                    }
                }
                else
                {
                    try
                    {
                        std::filesystem::remove(expandedPath);
                    }
                    catch (const std::filesystem::filesystem_error &e)
                    {
                        flog::error("[RECORDER] remove short file failed: {0}", e.what());
                    }
                }
            }

            if (status_direction && this_record)
            {
                curlPOST_end_new();
                status_direction = false;
            }
        }
        else
        {
            flog::info("[RECORDER] stop(): Already not recording. Skipping DSP/file close.");
        }

        recording.store(false);
        this_record = false;
        currWavFile.clear();
        akfState.store(AkfState::IDLE);
        analysisResultSignal.store(ANALYSIS_NONE);
        flog::info("[RECORDER] LEAVING stop() successfully.");
        isStopping.store(false);
        gui::mainWindow.setRecording(recording.load());
        flog::info("[RECORDER] Stop requested for '{0}'. Flag cleared.", name);
    }

private:
    std::atomic<AkfState> akfState{AkfState::IDLE};
    std::atomic<bool> restart_pending{false};
    std::mutex stopMtx;
    std::atomic<bool> isStopping{false};

    // Pre-recording buffer variables
    bool preRecord = true;
    int preRecordTimeMs = 500;
    bool preRecordUse = true; // New control variable
    size_t preBufferSizeInSamples = 0;
    LockFreeRingBuffer monoPreBuffer;

    static bool isControlledChannel(const std::string &name)
    {
        if (name.rfind("Запис C", 0) == 0 && name.length() > 12)
        {
            return isdigit(name[12]);
        }
        return false;
    }

    static int calcNumInst(const std::string &name)
    {
        if (name == "Запис C1")
            return 0;
        if (name == "Запис C2")
            return 1;
        if (name == "Запис C3")
            return 2;
        if (name == "Запис C4")
            return 3;
        if (name == "Запис C5")
            return 4;
        if (name == "Запис C6")
            return 5;
        if (name == "Запис C7")
            return 6;
        if (name == "Запис C8")
            return 7;
        return 0;
    }

    struct FileCloser
    {
        void operator()(FILE *f) const
        {
            if (f)
                pclose(f);
        }
    };

    std::string renameVaRootDir(const std::string &va_root_dir, int ChNumber)
    {
        if (ChNumber < 0 || ChNumber > 7)
        {
            flog::warn("ChNumber must be between 0 and 7");
            return va_root_dir;
        }

        std::string path = va_root_dir;
        if (!path.empty() && path.back() == '/')
            path.pop_back();

        std::regex pattern(R"(^(.*va_only-\d{2})\d$)");
        std::smatch match;

        if (std::regex_match(path, match, pattern))
        {
            return match[1].str() + std::to_string(ChNumber);
        }

        return va_root_dir;
    }

    int parseSixthBlock(const std::string &filename)
    {
        std::stringstream ss(filename);
        std::string part;
        std::vector<std::string> parts;

        while (std::getline(ss, part, '-'))
        {
            parts.push_back(part);
        }

        if (parts.size() < 6)
            return -1;

        try
        {
            return std::stoi(parts[5]);
        }
        catch (...)
        {
            return -1;
        }
    };

    json run_python_script_json(const std::string &script_path, const std::string &args)
    {
        std::string cmd = script_path + " " + args;
        std::array<char, 256> buffer;
        std::string result;

        std::unique_ptr<FILE, FileCloser> pipe(popen(cmd.c_str(), "r"));
        if (!pipe)
        {
            throw std::runtime_error("Failed to run script: popen() returned null");
        }

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        {
            result += buffer.data();
        }

        if (result.empty())
        {
            throw std::runtime_error("Script returned empty output!\nCMD: " + cmd);
        }

        json parsed;
        try
        {
            parsed = json::parse(result);
        }
        catch (const json::parse_error &e)
        {
            throw std::runtime_error("JSON parse error: " + std::string(e.what()) + "\nRaw output:\n" + result);
        }

        if (!parsed.contains("id") || !parsed.contains("filename"))
        {
            throw std::runtime_error("JSON does not contain required fields: 'id' and 'filename'\nRaw output:\n" + result);
        }

        if (parsed.value("id", 0) == 0 || parsed.value("filename", "").empty())
        {
            throw std::runtime_error("JSON contains invalid values (id == 0 or empty filename)\nRaw output:\n" + result);
        }

        return parsed;
    }

    void restart()
    {
        flog::info("[RECORDER] Restart requested for '{0}'.", name);
        restart_pending.store(true);
        stop(true);
    }

    static void clientHandler(net::Conn client, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;

        {
            std::lock_guard lck(_this->connMtx);
            _this->conn = std::move(client);
        }

        if (_this->conn)
        {
            _this->conn->waitForEnd();
            _this->conn->close();
        }

        _this->listener->acceptAsync(clientHandler, _this);
    }

    dsp::tap<float> generateLowpassTaps(float sampleRate, float cutoffFreq, int numTaps)
    {
        dsp::tap<float> taps = dsp::taps::alloc<float>(numTaps);

        float fc = cutoffFreq / sampleRate;

        for (int i = 0; i < numTaps; ++i)
        {
            int m = i - (numTaps - 1) / 2;
            float sinc = (m == 0) ? 2.0f * fc : sinf(2.0f * M_PI * fc * m) / (M_PI * m);
            float window = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (numTaps - 1));
            taps.taps[i] = sinc * window;
        }

        return taps;
    }

    typedef int Socket;

    std::string fixMissingQuotes(const std::string &raw)
    {
        std::string fixed = raw;
        fixed = std::regex_replace(fixed, std::regex(R"(\b([A-Za-z_][A-Za-z0-9_]*)\b(?=\s*:))"), R"("$1")");
        fixed = std::regex_replace(fixed, std::regex(R"(:\s*([A-Za-z_][A-Za-z0-9_]*))"), R"(: "$1")");
        fixed.erase(std::remove(fixed.begin(), fixed.end(), '\n'), fixed.end());
        fixed.erase(std::remove(fixed.begin(), fixed.end(), '\r'), fixed.end());
        return fixed;
    }

    json parseUdpData(const std::string &info)
    {
        try
        {
            return json::parse(info);
        }
        catch (const json::parse_error &e)
        {
            flog::warn("Ошибка парсинга JSON: {0}", e.what());
        }

        std::string fixed_data = fixMissingQuotes(info);
        try
        {
            if (!fixed_data.empty() && fixed_data[0] == '{' && fixed_data.back() == '}')
            {
                json parsed_data = json::parse(fixed_data);
                std::cout << "Парсинг успешен:\n"
                          << parsed_data.dump(4) << std::endl;
                return parsed_data;
            }
            else
            {
                std::cerr << "Данные не являются корректным JSON.\n";
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Ошибка парсинга JSON: " << e.what() << std::endl;
        }
        return nullptr;
    }

    bool makeDir(const char *dir)
    {
        if (fs::exists(dir))
            return fs::is_directory(fs::status(dir));
        else
            return fs::create_directories(dir);
    }

    void updatePreBufferSize()
    {
        if (!isControlledChannel(name))
        {
            preBufferSizeInSamples = 0;
            return;
        }

        uint64_t currentSamplerate = 0;
        if (recMode == RECORDER_MODE_AUDIO)
        {
            if (selectedStreamName.empty())
                return;
            currentSamplerate = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
        }
        else
        {
            preBufferSizeInSamples = 0;
            return;
        }

        if (currentSamplerate > 0)
        {
            preBufferSizeInSamples = (currentSamplerate * preRecordTimeMs) / 1000.0;
            flog::info("Pre-record buffer size updated: {0} samples for {1} ms at {2} Hz", preBufferSizeInSamples, preRecordTimeMs, currentSamplerate);
        }
    }

    static void menuHandler(void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;
        if (_this->name != "Запис")
            ImGui::BeginDisabled();
        if (_this->Admin)
        {
            style::beginDisabled();
            ImGui::BeginGroup();
            ImGui::Columns(3, CONCAT("RecorderModeColumns##_", _this->name), false);
            if (ImGui::RadioButton(CONCAT("СМО##_recorder_mode1_", _this->name), _this->recMode == RECORDER_MODE_BASEBAND))
            {
                _this->recMode = RECORDER_MODE_BASEBAND;
                config.acquire();
                config.conf[_this->name]["mode"] = _this->recMode;
                config.release(true);
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_FLOAT32);
                config.acquire();
                config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
                config.release(true);
            }

            ImGui::NextColumn();
            if (ImGui::RadioButton(CONCAT("Аудіо##_recorder_mode3_", _this->name), _this->recMode == RECORDER_MODE_AUDIO))
            {
                _this->recMode = RECORDER_MODE_AUDIO;
                config.acquire();
                config.conf[_this->name]["mode"] = _this->recMode;
                config.release(true);
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_INT16);
                config.acquire();
                config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
                config.release(true);
            }
            ImGui::Columns(1, CONCAT("EndRecorderModeColumns##_", _this->name), false);
            ImGui::EndGroup();
            style::endDisabled();
        }
        if (_this->recording.load())
        {
            style::beginDisabled();
        }

        if (_this->recording.load() && _this->saveInDir)
        {
            if (_this->folderSelect.render("##_recorder_folder_" + _this->name))
            {
                if (_this->folderSelect.pathIsValid())
                {
                    config.acquire();
                    config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                    config.release(true);
                }
            }
        }

        if (_this->radioMode > 1)
        {
            ImGui::LeftLabel("Формат назви файлу");
            ImGui::FillWidth();
            if (ImGui::InputText(CONCAT("##_recorder_name_template_", _this->name), _this->nameTemplate, 512))
            {
                config.acquire();
                config.conf[_this->name]["nameTemplate"] = _this->nameTemplate;
                config.release(true);
            }
        }

        config.conf[_this->name]["container"] = _this->containers.key(0);
        if (!_this->Admin)
            ImGui::BeginDisabled();
        ImGui::LeftLabel("Тип");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_st_", _this->name), &_this->sampleTypeId, _this->sampleTypes.txt))
        {
            config.acquire();
            config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
            config.release(true);
        }
        if (!_this->Admin)
            ImGui::EndDisabled();

        if (_this->recording.load())
        {
            style::endDisabled();
        }

        if (_this->recMode == RECORDER_MODE_AUDIO)
        {
            if (!_this->Admin)
                ImGui::BeginDisabled();

            ImGui::LeftLabel("Цифровий потік");
            ImGui::FillWidth();
            int old_streamId = _this->streamId;
            if (ImGui::Combo(CONCAT("##_recorder_stream_", _this->name), &_this->streamId, _this->audioStreams.txt))
            {
                bool _ok = false;
                if (_this->name == "Запис" && _this->strm_name == "Канал приймання")
                    _ok = true;
                else
                {
                    std::string Interface_name = "Запис " + _this->strm_name;
                    if (_this->name == Interface_name)
                        _ok = true;
                }

                if (_ok)
                {
                    _this->selectStream(_this->strm_name);

                    config.acquire();
                    if (config.conf[_this->name].contains("audioStream"))
                    {
                        _this->selectedStreamName = config.conf[_this->name]["audioStream"];
                    }
                    else
                    {
                        _this->selectedStreamName = "";
                    }

                    if (_this->selectedStreamName != _this->strm_name)
                    {
                        _this->selectedStreamName = _this->strm_name;
                        config.conf[_this->name]["audioStream"] = _this->audioStreams.key(_this->streamId);
                        config.release(true);
                    }
                    else
                    {
                        config.release();
                    }
                }
                else
                {
                    _this->streamId = old_streamId;
                }
            }
            if (!_this->Admin)
                ImGui::EndDisabled();

            _this->updateAudioMeter(_this->audioLvl);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.l, _this->audioLvl.l, -60, 10);
            ImGui::LeftLabel("Рівень запису");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_recorder_vol_", _this->name), &_this->audioVolume, 0, 1, "%0.2f"))
            {
                _this->volume.setVolume(_this->audioVolume);
                config.acquire();
                config.conf[_this->name]["audioVolume"] = _this->audioVolume;
                config.release(true);
            }
            _this->ignoreSilence = false;
        }
        if (_this->name != "Запис")
            ImGui::EndDisabled();

        bool canRecord = _this->folderSelect.pathIsValid();
        uint8_t currSrvr = gui::mainWindow.getCurrServer();
        if (_this->recMode == RECORDER_MODE_AUDIO)
        {
            canRecord &= !_this->selectedStreamName.empty();
        }
        if (_this->isARM)
        {
            bool run = gui::mainWindow.isServerIsNotPlaying(gui::mainWindow.getCurrServer());
            if (run)
                ImGui::BeginDisabled();
            if (!_this->recording.load())
            {
                bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();
                if (_ifStartElseBtn)
                {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(CONCAT("ЗАПИС на АРМ##_recorder_rec_startARM", _this->name), ImVec2(menuWidth, 0)))
                {
                    _this->akfState.store(AkfState::IDLE);
                    _this->currWavFile = "";
                    _this->this_record = true;
                    _this->start();
                    _this->processing = 1;
                    gui::mainWindow.setRecording(_this->recording.load());
                }
                if (_this->Admin)
                {
                    if (_this->flag_akf)
                    {
                        if (ImGui::Button(CONCAT("ЗАПИС (АКФ)##_recorder_rec_startAKF", _this->name), ImVec2(menuWidth, 0)))
                        {
                            _this->akfState.store(AkfState::RECORDING);
                            _this->this_record = true;
                            _this->start();
                            _this->processing = 1;
                            gui::mainWindow.setRecording(_this->recording.load());
                        }
                    }
                }
                if (_ifStartElseBtn)
                {
                    ImGui::EndDisabled();
                }
                if (run)
                    ImGui::EndDisabled();
            }
            else
            {
                if (ImGui::Button(CONCAT("ЗУПИНИТИ на АРМ##_recorder_rec_stop", _this->name), ImVec2(menuWidth, 0)))
                {
                    _this->stop();
                    _this->this_record = false;
                    gui::mainWindow.setRecording(_this->recording.load());
                    gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                    _this->processing = 0;
                }

                uint64_t seconds = _this->writer.getSamplesWritten() / _this->samplerate;
                time_t diff = seconds;
                tm *dtm = gmtime(&diff);

                if (_this->ignoreSilence)
                {
                    if (_this->ignoringSilence)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Запис (шум) %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Запис (сигнал) %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
                    }
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Запис %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
                }
            }
        }
        else // ifServer
        {
            if (!_this->recording.load())
            {
                bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();
                if (_ifStartElseBtn)
                {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(CONCAT("ЗАПИС##_recorder_rec_start", _this->name), ImVec2(menuWidth, 0)))
                {
                    _this->akfState.store(AkfState::IDLE);
                    _this->this_record = true;
                    _this->start();
                    gui::mainWindow.setServerRecordingStart(0);
                    _this->processing = 1;
                }
                if (_this->Admin)
                {
                    if (_this->flag_akf)
                    {
                        if (ImGui::Button(CONCAT("ЗАПИС (АКФ)##_recorder_rec_startAKF", _this->name), ImVec2(menuWidth, 0)))
                        {
                            _this->akfState.store(AkfState::RECORDING);
                            _this->this_record = true;
                            _this->start();
                            gui::mainWindow.setServerRecordingStart(0);
                            gui::mainWindow.setUpdateMenuSnd3Record(true);
                        }
                    }
                }
                if (_ifStartElseBtn)
                {
                    ImGui::EndDisabled();
                }
            }
            else
            {
                if (ImGui::Button(CONCAT("ЗУПИНИТИ##_recorder_rec_stop", _this->name), ImVec2(menuWidth, 0)))
                {
                    _this->stop();
                    gui::mainWindow.setServerRecordingStop(0);
                    _this->processing = 0;
                    _this->this_record = false;
                }
                uint64_t seconds = _this->writer.getSamplesWritten() / _this->samplerate;
                if (_this->recMode == RECORDER_MODE_PUREIQ)
                    seconds = seconds;
                time_t diff = seconds;
                tm *dtm = gmtime(&diff);
                if (_this->ignoreSilence && _this->ignoringSilence)
                {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Запис (шум) %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Запис (сигнал) %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
                }
                if (_this->flag_akf)
                {
                    bool listening = (_this->listener && _this->listener->isListening()) || (_this->conn && _this->conn->isOpen());

                    ImGui::TextUnformatted("Статус:");
                    ImGui::SameLine();
                    if (_this->conn && _this->conn->isOpen())
                    {
                        ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), (_this->modeId == SINK_MODE_TCP) ? "З'єднання" : "Надсилання");
                    }
                    else if (listening)
                    {
                        ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Прослуховування");
                    }
                    else
                    {
                        ImGui::TextUnformatted("Очікування");
                    }
                }
            }
        }
        if (_this->isARM)
        {
            if (gui::mainWindow.getServerStatus(currSrvr) > 1)
            {
                if (!gui::mainWindow.getServerRecording(currSrvr))
                {
                    bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();
                    if (_ifStartElseBtn)
                    {
                        ImGui::BeginDisabled();
                    }

                    if (ImGui::Button(CONCAT("ЗАПИС##_recorder_srvARM2", _this->name), ImVec2(menuWidth, 0)))
                    {
                        _this->akfState.store(AkfState::IDLE);
                        gui::mainWindow.setRecording(true);
                        gui::mainWindow.setServerRecordingStart(currSrvr);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrvr, true);
                        gui::mainWindow.setUpdateMenuSnd3Record(true);
                    }
                    if (_ifStartElseBtn)
                    {
                        ImGui::EndDisabled();
                    }
                }
                else
                {
                    if (ImGui::Button(CONCAT("ЗУПИНИТИ##_recorder_srv_stop", _this->name), ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setRecording(false);
                        gui::mainWindow.setServerRecordingStop(currSrvr);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrvr, true);
                        gui::mainWindow.setUpdateMenuSnd3Record(true);
                        _this->processing = 0;
                    }
                }
            }
        }
    }

#define PORT 20000
#define MAXLINE 1024

    void startServer(int interrupt_fd)
    {
        analysisResultSignal.store(ANALYSIS_PENDING);
        flog::info("startServer: Waiting for analysis result for file {0}", currWavFile);

        if (host.empty())
        {
            flog::error("Host string is empty. Cannot initialize listener.");
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
            return;
        }

        int WAIT_SEC = maxRecShortDur_sec + 4;
        akf_timeout_sec = WAIT_SEC;

        int vaport = akfUdpPort;
        flog::info("[AKF WAIT] inst='{0}' port={1} file='{2}' dir='{3}'", name.c_str(), akfUdpPort, currWavFile.c_str(), shortRecDirectory.c_str());

        if (akfUdpPort <= 0)
        {
            flog::error("[AKF WAIT {0}] Invalid AKF UDP port: {1}. Disabling AKF for this take.",
                        name.c_str(), akfUdpPort);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
            return;
        }

        std::string info = net::listenUDP(host, vaport, akf_timeout_sec, interrupt_fd);
        akf_timeout_sec = 0;

        if (info == "interrupted")
        {
            flog::info("Analysis for '{0}' was interrupted by a stop signal.", name);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        else if (info.rfind("error", 0) == 0)
        {
            flog::error("listenUDP for '{0}' failed with: {1}", name, info);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        else if (info.empty())
        {
            flog::warn("listenUDP for '{0}' returned empty data.", name);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        else
        {
            flog::info("AKF json = {0}", info);
            json import = parseUdpData(info);
            if (!import.is_null())
            {
                try
                {
                    int localSignal = import["Signal"].get<int>();
                    if (localSignal > 0)
                    {
                        if (localSignal == 2 && import.contains("Comment"))
                        {
                            if (import["Comment"] == "HB")
                            {
                                localSignal = 3; // NV
                            }
                            else if (import["Comment"] != "DMR")
                            {
                                localSignal = 4;
                            }
                        }
                        flog::info("Analysis result: SIGNAL DETECTED with value {0} for {1}", localSignal, currWavFile);
                        analysisResultSignal.store(localSignal);
                    }
                    else
                    {
                        flog::info("Analysis result: NOISE DETECTED for {0}", currWavFile);
                        analysisResultSignal.store(ANALYSIS_NOISE_OR_TIMEOUT);
                    }
                }
                catch (const std::exception &e)
                {
                    flog::warn("Error parsing JSON, considering it as noise/timeout. Error: {0}", e.what());
                    analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
                }
            }
            else
            {
                flog::info("Could not parse JSON, considering it as noise/timeout.");
                analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
            }
        }
    }

    void runAnalysisTask()
    {
        pleaseStopAnalysis.store(false);
        drainShutdownPipe(shutdownPipeFd[0]);
        analysisResultSignal.store(ANALYSIS_PENDING);
        restart_pending.store(false);
        akf_timeout_sec = 0;

        std::thread oldThread;
        {
            std::lock_guard<std::mutex> lk(analysisThreadMtx);
            wakeShutdownPipe(shutdownPipeFd[1]);
            if (analysisThread.joinable())
                oldThread = std::move(analysisThread);
            analysisSocketFd.store(-1);
            drainShutdownPipe(shutdownPipeFd[0]);
        }

        if (oldThread.joinable())
            oldThread.join();

        analysisThread = std::thread([this]()
                                     {
            flog::info("Starting analysis thread for '{0}'...", this->name);

            struct Guard {
                RecorderModule* self;
                bool finished_ok = false;
                ~Guard() {
                    if (!self) return;
                    if (!finished_ok) {
                        self->analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
                    }
                    self->analysisSocketFd.store(-1);
                }
            } guard{this, false};

            try {
                const int interrupt_fd = this->shutdownPipeFd[0];
                if (interrupt_fd < 0) {
                    flog::warn("startServer: interrupt_fd is invalid (<0); proceeding without interrupt.");
                }            
                this->startServer(interrupt_fd);
                guard.finished_ok = true;
            }
            catch (const std::exception& e) {
                flog::error("An exception in analysis thread for '{0}': {1}", this->name, e.what());
            }

            flog::info("Analysis thread for '{0}' finished.", this->name); });
    }

    void selectStream(std::string name)
    {
        std::string _name = std::string(name);
        {
            std::unique_lock<std::recursive_mutex> lck(recMtx);
            deselectStream();
            if (audioStreams.empty())
            {
                selectedStreamName.clear();
                return;
            }
            else if (!audioStreams.keyExists(_name))
            {
                flog::warn("TRACE! RECORD! ERROR _name {0}", _name);
                selectStream(audioStreams.key(0));
                return;
            }
        }
        audioStream = sigpath::sinkManager.bindStream(_name);
        if (!audioStream)
        {
            return;
        }
        {
            std::unique_lock<std::recursive_mutex> lck(recMtx);
            selectedStreamName = _name;
            streamId = audioStreams.keyId(_name);
            volume.setInput(audioStream);
            startAudioPath(); // This now starts pre-record components as well

            // Start pre-recording sink if applicable
            flog::info("preRecord {0} && isControlledChannel({1}) = {2}  && recMode == RECORDER_MODE_AUDIO ({3})", preRecord, this->name, isControlledChannel(this->name), recMode == RECORDER_MODE_AUDIO);
            if (preRecord && isControlledChannel(this->name) && recMode == RECORDER_MODE_AUDIO)
            {
                updatePreBufferSize();
                flog::info("Pre-record sink started for {0}", this->name);
            }
        }
        flog::info("TRACE! RECORD! selectStream  _name{0}", _name);
    }

    void deselectStream()
    {
        if (selectedStreamName.empty() || !audioStream)
        {
            selectedStreamName.clear();
            return;
        }

        if (recording.load() && recMode == RECORDER_MODE_AUDIO)
        {
            stop();
            gui::mainWindow.setServerRecordingStop(0);
        }
        {
            std::lock_guard<std::recursive_mutex> lck(recMtx);
            stopAudioPath();

            sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);

            float dummy;
            while (monoPreBuffer.pop(dummy))
            {
                // Просто вычитываем и игнорируем
            }
        }
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath()
    {
        volume.start();
        stereoSplitter.start();
        meter.start();
        s2m.start();
        monoSplitter.start();
        monoSink.start();
        preRecordSink.start();
    }

    void stopAudioPath()
    {
        volume.stop();
        stereoSplitter.stop();
        meter.stop();
        s2m.stop();
        monoSplitter.stop();
        monoSink.stop();
        preRecordSink.stop();
    }

    static void streamRegisteredHandler(std::string name, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        _this->audioStreams.define(name, name, name);
        if (_this->selectedStreamName.empty())
        {
            _this->selectStream(name);
        }
        else
        {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        _this->audioStreams.undefineKey(name);
        if (_this->selectedStreamName == name)
        {
            _this->selectStream("");
        }
        else
        {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    void updateAudioMeter(dsp::stereo_t &lvl)
    {
        double frameTime = 1.0 / ImGui::GetIO().Framerate;
        lvl.l = std::clamp<float>(lvl.l - (frameTime * 50.0), -90.0f, 10.0f);
        lvl.r = std::clamp<float>(lvl.r - (frameTime * 50.0), -90.0f, 10.0f);
        dsp::stereo_t rawLvl = meter.getLevel();
        meter.resetLevel();
        dsp::stereo_t dbLvl = {10.0f * logf(rawLvl.l), 10.0f * logf(rawLvl.r)};
        if (dbLvl.l > lvl.l)
        {
            lvl.l = dbLvl.l;
        }
        if (dbLvl.r > lvl.r)
        {
            lvl.r = dbLvl.r;
        }
    }

#include <chrono>

    std::string expandString(std::string input)
    {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    const float SILENCE_LVL = 10e-5;
    const float NOISE_REDUCTION_FACTOR = 0.05f;
    const float NOISE_THRESHOLD = 0.2f * SILENCE_LVL;
    const float NOISE_LVL = 0.5f;
    const float NOISE_SUPPRESSION_EXPONENT = 0.5f;
    const int CALIBRATION_FRAMES = 1000;
    const float NOISE_LVL_INITIAL = 0.4f;
    const float SIGNAL_LVL_INITIAL = 0.19f;
    const float MIN_NOISE_LEVEL = 0.0005f;
    const int RMS_HISTORY_SIZE = 20;
    const float RMS_SMOOTHING_FACTOR = 0.1f;

    inline float adaptiveNoiseReduction(float rms)
    {
        if (rms > NOISE_LVL)
            return MIN_NOISE_LEVEL;
        if (rms > signal_lvl)
        {
            float normalized = (NOISE_LVL - rms) / (NOISE_LVL - signal_lvl);
            normalized = fmaxf(0.0f, fminf(normalized, 1.0f));
            return MIN_NOISE_LEVEL + (1.0f - MIN_NOISE_LEVEL) * powf(normalized, NOISE_SUPPRESSION_EXPONENT);
        }
        return 1.0f;
    }

    inline float noiseReductionFactorThree(float rms)
    {
        return (rms <= SILENCE_LVL) ? 1.0f : powf(SILENCE_LVL / rms, 3.0f);
    }

    inline float noiseReductionFactorQr(float rms)
    {
        return (rms >= SILENCE_LVL) ? 1.0f : powf(rms / SILENCE_LVL, 2.0f);
    }

    inline bool wantAKF() const
    {
        if (recMode != RECORDER_MODE_AUDIO)
            return false;
        if (!flag_akf)
            return false;
        return akfState.load() == AkfState::RECORDING;
    }

    bool akfPreflightOk()
    {
        try
        {
            std::string akf_final_dir = shortRecDirectory;
            std::string akf_tmp_dir = shortRecDirectory + "/../tmp_recording";

            std::filesystem::create_directories(akf_final_dir);
            std::filesystem::create_directories(akf_tmp_dir);

            int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s >= 0)
            {
                int one = 1;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(akfUdpPort);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::bind(s, (sockaddr *)&addr, sizeof(addr)) != 0)
                {
                    int e = errno;
                    ::close(s);
                    flog::error("[RECORDER {0}] [AKF] preflight: UDP port {1} not free: {2}", name.c_str(), akfUdpPort, strerror(e));
                    return false;
                }
                ::close(s);
            }
            return true;
        }
        catch (const std::exception &e)
        {
            flog::error("[RECORDER {0}] [AKF] preflight failed: {1}", name.c_str(), e.what());
            return false;
        }
    }
    void deferredStop(bool flag_rename)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        this->stop_akf(false);
        this->stop(flag_rename);
    }

    void processAkfSuccess(int signalValue)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        flog::info("[PROCESSOR] Processing successful AKF result with signal {0}", signalValue);
        std::lock_guard lck(recMtx);

        if (!recording.load())
        {
            flog::warn("[PROCESSOR] Recording was stopped before AKF success could be processed. Aborting.");
            return;
        }
        if (insert_REC.load())
        {
            flog::warn("[PROCESSOR] AKF success has already been processed. Ignoring duplicate call.");
            return;
        }

        Signal = signalValue;
        status_direction = true;
        insert_REC.store(true);

        if (!insert_REC_PGDB(this->currWavFile))
        {
            flog::error("[PROCESSOR] FAILED to create record in DB for {0}", this->currWavFile);
        }
        else
        {
            flog::info("[PROCESSOR] Record in DB for new file name '{0}' created successfully!", this->currWavFile);
            this->folderSelect.setPath(this->wavPath);
            this->curr_expandedPath = this->expandString(this->folderSelect.path + "/" + this->currWavFile);
            flog::info("[PROCESSOR] Starting POST request for {0}", this->currWavFile);
            if (this->this_record)
                this->curlPOST_begin_new();
        }
    }

    void initiateSuccessfulRecording(int signalValue)
    {
        // Шаг 1: Под блокировкой быстро читаем и меняем все необходимые переменные
        std::string file_to_process;
        bool should_run_post = false;

        { // Начало критической секции
            std::lock_guard lck(recMtx);
            if (insert_REC.load())
            {
                flog::warn("[INITIATOR] Recording already stopped or initiated. Aborting.");
                return;
            }
            flog::info("[INITIATOR] Initiating successful recording with signal {0}", signalValue);

            Signal = signalValue;
            status_direction = true;
            insert_REC.store(true);

            // Копируем данные, необходимые для долгих операций
            file_to_process = this->currWavFile;
            should_run_post = this->this_record;

        } // Конец критической секции. recMtx освобожден.

        // Шаг 2: Выполняем долгие, блокирующие операции БЕЗ блокировки
        if (!insert_REC_PGDB(file_to_process))
        {
            flog::error("[INITIATOR] FAILED to create record in DB for {0}", file_to_process);
        }
        else
        {
            flog::info("[INITIATOR] Record in DB for new file name '{0}' created successfully!", this->currWavFile);

            // Эти операции тоже должны быть вне блокировки, если они могут быть долгими
            this->folderSelect.setPath(this->wavPath);
            this->curr_expandedPath = this->expandString(this->folderSelect.path + "/" + this->currWavFile);

            flog::info("[INITIATOR] Starting POST request for {0}", this->currWavFile);
            if (should_run_post)
                this->curlPOST_begin_new();
        }
    }

    void initiateSuccessfulRecording2(int signalValue)
    {
        std::lock_guard lck(recMtx);
        if (insert_REC.load())
        {
            flog::warn("[INITIATOR] Recording already stopped or initiated. Aborting.");
            return;
        }
        flog::info("[INITIATOR] Initiating successful recording with signal {0}", signalValue);
        Signal = signalValue;
        status_direction = true;
        insert_REC.store(true);

        if (!insert_REC_PGDB(this->currWavFile))
        {
            flog::error("[INITIATOR] FAILED to create record in DB for {0}", this->currWavFile);
        }
        else
        {
            flog::info("[INITIATOR] Record in DB for new file name '{0}' created successfully!", this->currWavFile);
            this->folderSelect.setPath(this->wavPath);
            this->curr_expandedPath = this->expandString(this->folderSelect.path + "/" + this->currWavFile);
            flog::info("[INITIATOR] Starting POST request for {0}", this->currWavFile);
            if (this->this_record)
                this->curlPOST_begin_new();
        }
    }

    static void preRecordMonoHandler(float *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (!data || count <= 0 || !_this->recording.load())
        {
            return;
        }

        if (_this->preRecord && isControlledChannel(_this->name))
        {
            // НЕТ БОЛЬШЕ НИКАКИХ МЬЮТЕКСОВ!
            for (int i = 0; i < count; ++i)
            {
                _this->monoPreBuffer.push(data[i]);
            }
        }
    }

    static void monoHandler(float *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (!data || count <= 0)
            return;
        if (!_this->recording.load())
            return;

        if (_this->akfState.load() == AkfState::ANALYSIS_PENDING)
        {
            int resultSignal = _this->analysisResultSignal.load();
            if (resultSignal != ANALYSIS_PENDING)
            {
                if (resultSignal == ANALYSIS_NOISE_OR_TIMEOUT)
                {
                    _this->akfState.store(AkfState::NOISE_DETECTED);
                    flog::info("[MONO_HANDLER] Analysis result: NOISE/TIMEOUT. State -> NOISE_DETECTED.");
                }
                else
                {
                    _this->Signal = resultSignal;
                    _this->akfState.store(AkfState::SIGNAL_DETECTED);
                    flog::info("[MONO_HANDLER] Analysis result: SIGNAL {0}. State -> SIGNAL_DETECTED.", resultSignal);
                }
                _this->analysisResultSignal.store(ANALYSIS_NONE);
            }
        }

        _this->writer.write(data, count);

        if (_this->akfState.load() == AkfState::RECORDING)
        {
            if (_this->writer_akf && _this->writer_akf->isOpen())
            {
                _this->writer_akf->write(data, count);
                uint64_t samples_written_akf = _this->writer_akf->getSamplesWritten();
                if ((samples_written_akf / _this->samplerate) >= _this->maxRecShortDur_sec)
                {
                    _this->akfState.store(AkfState::ANALYSIS_PENDING);
                    flog::info("[MONO_HANDLER] Short recording phase finished. State -> ANALYSIS_PENDING.");
                }
            }
        }

        if (!_this->_restart.load())
        {
            uint64_t seconds_main = _this->writer.getSamplesWritten() / _this->samplerate;
            if (seconds_main >= _this->maxRecDuration * 60)
            {
                flog::info("[MONO_HANDLER] Max recording duration ({0}s) reached. Triggering restart.", seconds_main);
                _this->_restart.store(true);
            }
        }
    }

    static void stereoHandler(dsp::stereo_t *data, int count, void *ctx)
    {
        // This handler is not used for recording in the current mono-only logic
        // but is kept for potential future use.
    }

    static void complexHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (!_this->recording.load())
            return;

        uint64_t seconds = _this->writer.getSamplesWritten() / _this->samplerate;
        if (_this->_restart == false)
        {
            if (seconds >= _this->maxRecDuration * 60)
            {
                flog::info("seconds {0}, maxRecDuration {1}, radioMode {2}", seconds, _this->maxRecDuration, _this->radioMode);
                _this->_restart = true;
                flog::info("RESTART =  {0}", _this->_restart.load());
            }
        }
        _this->writer.write((float *)data, count);
    }

    static void dummyHandler(dsp::stereo_t *, int, void *)
    {
    }

    static void iqHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RecorderModule *_this = static_cast<RecorderModule *>(ctx);
        _this->writer.write(reinterpret_cast<float *>(data), count * 2);
    }

    static void moduleInterfaceHandler(int code, void *in, void *out, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (code == RECORDER_IFACE_CMD_STOP)
        {
            flog::info("RECORDER_IFACE_CMD_STOP: name {0}, _this->recording {1}", _this->name, _this->recording.load());
            _this->stop(true);
            _this->akfState.store(AkfState::IDLE);
            return;
        }
        std::lock_guard lck(_this->recMtx);

        if (code == RECORDER_IFACE_GET_SIGNAL)
        {
            int *_out = (int *)out;
            *_out = _this->Signal;
        }
        else if (code == RECORDER_IFACE_GET_AKF_TIMEOUT)
        {
            if (out)
            {
                *(int *)out = _this->akf_timeout_sec;
            }
        }
        else if (code == RECORDER_IFACE_GET_RECORD)
        {
            int *_out = (int *)out;
            *_out = _this->recording.load();
        }
        else if (code == RECORDER_IFACE_CMD_START_AKF)
        {
            int *_in = (int *)in;
            int _rm = std::clamp<int>(*_in, 0, 10);
            AkfState old_state = _this->akfState.load();
            if (_this->flag_akf)
            {
                if (_rm > 0)
                    _this->akfState.store(AkfState::RECORDING);
                else
                    _this->akfState.store(AkfState::IDLE);
            }
            else
            {
                _this->akfState.store(AkfState::IDLE);
            }
            flog::info("[IFACE] START_AKF: State changed from {0} to {1}. _this->flag_akf {2}", akfStateToString(old_state), akfStateToString(_this->akfState.load()), _this->flag_akf);
        }
        else if (code == RECORDER_IFACE_CMD_GET_MODE)
        {
            int *_out = (int *)out;
            *_out = _this->recMode;
        }
        else if (code == RECORDER_IFACE_CMD_SET_FREQ)
        {
            double *_in = (double *)in;
            _this->current = *_in;
            flog::info("RECORDER_IFACE_CMD_SET_FREQ: _this->current {0}", _this->current);
        }
        else if (code == RECORDER_IFACE_CMD_SET_MODE)
        {
            int *_in = (int *)in;
            int _rm = std::clamp<int>(*_in, 0, 2);
            if (_rm != _this->recMode)
            {
                _this->recMode = _rm;
                config.acquire();
                config.conf[_this->name]["mode"] = _this->recMode;
                config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
                config.release(true);
                flog::info("RECORDER_IFACE_CMD_SET_MODE: _this->sampleTypeId {0}, _this->recMode {1}", _this->sampleTypeId, _this->recMode);
            }
            if (_this->recMode == RECORDER_MODE_AUDIO)
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_INT16);
            else
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_FLOAT32);
        }
        else if (code == RECORDER_IFACE_CMD_START)
        {
            int *_in = (int *)in;
            std::string s((char *)_in);
            _this->currWavFile = _this->expandString(s);
            flog::info("RECORDER_IFACE_CMD_START: name {0}, recording {1},  file = {2}, _this->akfState {3}", _this->name, _this->recording.load(), _this->currWavFile, akfStateToString(_this->akfState.load()));
            if (!_this->recording.load())
            {
                _this->start();
            }
        }
        else if (code == RECORDER_IFACE_GET_TEMPL)
        {
            std::string *_out = (std::string *)out;
            std::string currWavFile = std::string(_this->nameTemplate) + ".wav";
            *_out = currWavFile.c_str();
        }
        else if (code == RECORDER_IFACE_CMD_SET_STREAM)
        {
            std::string strm_name = "";
            if (_this->name == "Запис")
                strm_name = "Канал приймання";
            else
            {
                if (_this->name == "Запис C1")
                    strm_name = "C1";
                else if (_this->name == "Запис C2")
                    strm_name = "C2";
                else if (_this->name == "Запис C3")
                    strm_name = "C3";
                else if (_this->name == "Запис C4")
                    strm_name = "C4";
                else if (_this->name == "Запис C5")
                    strm_name = "C5";
                else if (_this->name == "Запис C6")
                    strm_name = "C6";
                else if (_this->name == "Запис C7")
                    strm_name = "C7";
                else if (_this->name == "Запис C8")
                    strm_name = "C8";
                else
                    strm_name = "";
            }
            if (strm_name == "")
                strm_name = "Канал приймання";
            if (_this->selectedStreamName != strm_name)
            {
                _this->selectedStreamName = std::string(strm_name);
                config.acquire();
                config.conf[_this->name]["audioStream"] = strm_name;
                config.release(true);
            }
            _this->selectStream(strm_name);
            _this->volume.setMuted(false);
            flog::info("RECORDER_IFACE_CMD_SET_STREAM _this->streamId {0}, strm_name {1}", _this->streamId, strm_name);
            _this->selectStream(_this->audioStreams.value(_this->streamId));
        }
        else if (code == MAIN_SET_START)
        {
            _this->processing = 1;
        }
        else if (code == MAIN_SET_STOP)
        {
            _this->processing = 0;
        }
        else if (code == MAIN_GET_PROCESSING)
        {
            int *_out = (int *)out;
            *_out = _this->processing;
        }
        else if (code == MAIN_SET_STATUS_CHANGE)
        {
            _this->changing = 1;
            flog::info("MAIN_SET_STATUS_CHANGE {0}", _this->changing);
        }
        else if (code == MAIN_GET_STATUS_CHANGE)
        {
            int *_out = (int *)out;
            if (_this->changing == 1)
            {
                *_out = 1;
                _this->changing = 0;
            }
            else
            {
                *_out = 0;
            }
        }
    }

    int safe_to_int(const std::string &s)
    {
        try
        {
            return std::stoi(s);
        }
        catch (...)
        {
            return 0;
        }
    }

    std::string build_args_from_filename(const std::string &filename)
    {
        std::string name = std::filesystem::path(filename).stem().string();
        std::vector<std::string> parts;
        std::stringstream ss(name);
        std::string item;

        while (std::getline(ss, item, '-'))
        {
            parts.push_back(item);
        }
        if (parts.size() < 7)
        {
            throw std::runtime_error("Некорректное имя файла, ожидается минимум 7 полей");
        }

        std::string unixtime = parts[1];
        std::string freq = parts[2];
        std::string band = parts[3];

        int receiver_num = 0;
        for (char c : parts[4])
        {
            if (std::isdigit(c))
                receiver_num = receiver_num * 10 + (c - '0');
        }
        std::string receiver = std::to_string(receiver_num);

        std::string radiomode = parts[5];
        std::string modulation = parts[6];

        std::string new_filename = parts[0] + "-" + unixtime + "-" + freq + "-" + band + "-" +
                                   receiver + "-" + radiomode + "-" + modulation + ".wav";

        std::string args = "\"" + new_filename + "\" " + unixtime + " " + freq + " " +
                           modulation + " " + radiomode + " " + band + " " + receiver;

        return args;
    }

    bool insert_REC_PGDB(const std::string &oldfile)
    {
        bool isRunning = false;
        try
        {
            std::string args = "/opt/avr/app/transcription/db_rpm.py " + build_args_from_filename(oldfile);
            json result = run_python_script_json("/opt/avr/python/epython3", args);

            int id = result.value("id", 0);
            std::string filename = result.value("filename", "");

            if (id == 0 || filename.empty())
            {
                std::cerr << "[ERROR] Script returned empty or invalid result\n";
                isRunning = false;
                return false;
            }

            currWavFile = filename;
            isRunning = true;
            std::cout << "[OK] Запис у базу: ID = " << id << ", файл = " << filename << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[ERROR] " << e.what() << std::endl;
            isRunning = false;
            return false;
        }
    }
    bool curlPOST_begin_new()
    {
        std::string url = "https://127.0.0.1:48601/";
        double band = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end())
        {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0);
        oss << "?type=lotus_start_pelengation"
            << "&user=user1"
            << "&token=3K8jj0iWGMYMTWCzjfl8"
            << "&ln=en"
            << "&freq=" << freq
            << "&band=" << band
            << "&timeout=-1"
            << "&npost=1"
            << "&;";
        std::string payload = oss.str();

        std::thread([url, payload]()
                    {
        try {
            CURL* curl = curl_easy_init();
            if (curl) {
                char curlErrorBuffer[CURL_ERROR_SIZE]{};
                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 750L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 400L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    flog::warn("curlPOST_begin_new curl_easy_perform() failed: {0}", curl_easy_strerror(res));
                } else {
                    flog::info("curlPOST_begin_new POST success to {0} with payload {1}", url, payload);
                }

                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code != 200) {
                    flog::warn("Server returned HTTP code: {0}", http_code);
                }
                curl_easy_cleanup(curl);
            } else {
                flog::error("curl_easy_init() failed");
            }
        } catch (const std::exception& e) {
            flog::error("curlPOST_begin_new exception: {0}", e.what());
        } })
            .detach();

        return false;
    }

    bool curlPOST_end_new()
    {
        if (!status_direction)
            return false;

        std::string url = "https://127.0.0.1:48601/";
        std::string payload = "?type=lotus_stop_pelengation&user=user1&token=3K8jj0iWGMYMTWCzjfl8&ln=en&";

        std::thread([url, payload]()
                    {
        try {
            CURL* curl = curl_easy_init();
            if (curl) {
                char curlErrorBuffer[CURL_ERROR_SIZE]{};
                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 750L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 400L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    flog::warn("curlPOST_end_new curl_easy_perform() failed: {0}", curl_easy_strerror(res));
                } else {
                    flog::info("curlPOST_end_new POST success to {0} with payload {1}", url, payload);
                }

                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code != 200) {
                    flog::warn("Server returned HTTP code: {0}", http_code);
                }
                curl_easy_cleanup(curl);
            } else {
                flog::error("curl_easy_init() failed");
            }
        } catch (const std::exception& e) {
            flog::error("curlPOST_end_new exception: {0}", e.what());
        } })
            .detach();

        return false;
    }

    enum
    {
        SINK_MODE_TCP,
        SINK_MODE_UDP
    };
    enum StopReason
    {
        NONE = 0,
        FROM_NOISE = 1,
        FROM_GUI = 2
    };
    enum class AkfDecision
    {
        PENDING,
        CONTINUE,
        STOP
    };
    std::atomic<AkfDecision> akfDecision;

    static constexpr int ANALYSIS_NONE = -2;
    static constexpr int ANALYSIS_PENDING = -1;
    static constexpr int ANALYSIS_NOISE_OR_TIMEOUT = 0;
    static constexpr int ANALYSIS_ERROR_OR_TIMEOUT = 3;

    std::atomic<int> analysisResultSignal{ANALYSIS_PENDING};

    bool saveInDir = false;
    std::string wavPath = "/opt/recordings/";
    std::string expandedPath;
    std::string expandedPath_akf;
    std::string curr_expandedPath;
    std::string curr_expandedPath_akf;

    bool enabled = true;
    std::string root;
    char nameTemplate[1024];

    OptionList<std::string, wav::Format> containers;
    OptionList<int, wav::SampleType> sampleTypes;
    OptionList<int, SamplingRate> samplingRates;
    int sampleTypeId = 1;
    int samplingRateId = 0;
    FolderSelect folderSelect;
    FolderSelect folderSelect_akf;

    int recMode = RECORDER_MODE_AUDIO;
    int containerId;
    bool stereo = false;
    std::string selectedStreamName = "";
    float audioVolume = 0.5f;
    bool ignoreSilence = false;
    dsp::stereo_t audioLvl = {-100.0f, -100.0f};

    std::atomic<bool> recording{false};
    bool ignoringSilence = false;
    wav::Writer writer;
    wav::Writer *writer_akf = nullptr;
    std::recursive_mutex recMtx;

    dsp::stream<dsp::complex_t> *basebandStream;
    // --- Start of Corrected DSP Chain Members ---
    dsp::stream<dsp::stereo_t> stereoStream; // Dummy for stereoSink, not used for mono path
    dsp::stream<dsp::stereo_t> meterStream;
    dsp::stream<dsp::stereo_t> s2mStream;
    dsp::stream<float> monoSinkStream;
    dsp::stream<float> preRecordSinkStream;
    dsp::routing::Splitter<dsp::stereo_t> stereoSplitter;
    dsp::routing::Splitter<float> monoSplitter;
    // --- End of Corrected DSP Chain Members ---

    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;
    dsp::sink::Handler<float> monoSink;
    dsp::sink::Handler<float> preRecordSink;

    dsp::stream<dsp::complex_t> *narrowIQStream = nullptr;
    dsp::filter::DecimatingFIR<dsp::complex_t, float> *decimator = nullptr;
    dsp::tap<float> narrowTaps;
    std::thread runThread;

    dsp::sink::Handler<dsp::complex_t> iqSink;
    dsp::sink::Handler<dsp::stereo_t> dummySink;

    dsp::stream<dsp::complex_t> *iqInputStream = nullptr;
    dsp::demod::pureIQ<dsp::complex_t> *iqFilter = nullptr;
    dsp::sink::Handler<dsp::complex_t> pureIQSink;

    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t> *audioStream = NULL;
    dsp::audio::Volume volume;
    dsp::bench::PeakLevelMeter<dsp::stereo_t> meter;
    dsp::convert::StereoToMono s2m;

    uint64_t samplerate = 8000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

    std::string currWavFile = "";
    double current = 88000000.0;

    int processing = 0;
    int changing = 0;

    bool isARM = false;
    bool isServer = false;
    bool isControl = false;
    std::string currSource;
    uint8_t CurrSrvr;

    std::atomic<bool> _restart{false};
    std::atomic<bool> insert_REC{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> stop_request_reason{StopReason::NONE};
    int radioMode = 0;
    int maxRecDuration = 10;
    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";
    bool status_direction = false;
    int baseband_band = 1000000;

    bool flag_akf = false;
    int maxRecShortDur_sec = 3;
    std::string shortRecDirectory = "/var/lib/avr/cws/data/receiver/va_only";
    std::string longRecDirectory = "/var/lib/avr/cws/data/receiver/records";

    std::string host = "localhost";
    char hostname[1024];
    int SIport = 63001;

    int modeId = SINK_MODE_UDP;
    net::Listener listener;
    net::Conn conn;
    std::mutex connMtx;
    bool startFirst = false;

    int Signal = -1;

    int NUM_INST = 0;
    bool initialized = false;
    std::vector<float> rms_history;
    float noise_lvl = NOISE_LVL_INITIAL;
    float signal_lvl = SIGNAL_LVL_INITIAL;
    float smoothed_rms = NOISE_LVL_INITIAL;
    std::string strm_name = "";
    std::string mainWavPath = "";
    bool use_curl = false;
    bool Admin = false;
    bool this_record = false;
    bool isSplitterBound = false;
    std::thread analysisThread;
    std::mutex analysisThreadMtx;
    std::atomic<int> analysisSocketFd{-1};
    int shutdownPipeFd[2] = {-1, -1};
    int akf_timeout_sec = 0;
    std::atomic<bool> pleaseStopAnalysis{false};
    std::atomic<bool> is_destructing{false};
    int akfUdpPort = 0;
};

std::atomic<bool> RecorderModule::g_stop_workers{false};

MOD_EXPORT void _INIT_()
{
    RecorderModule::g_stop_workers.store(false);

    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings"))
    {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings"))
        {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/recorder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name)
{
    return new RecorderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance *inst)
{
    delete (RecorderModule *)inst;
}

MOD_EXPORT void _END_()
{
    RecorderModule::g_stop_workers.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    config.disableAutoSave();
    config.save();
}