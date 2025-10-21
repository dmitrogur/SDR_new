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
    NOISE_DETECTED,   // Обнаружен шум/таймаут, основная запись будет остановлена
    AWAITING_PRERECORD_RESTART
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
    case AkfState::AWAITING_PRERECORD_RESTART:
        return "AWAITING_PRERECORD_RESTART";
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
    LockFreeRingBuffer(size_t size) : _size(size), _head(0), _tail(0)
    {
        _buffer = new std::atomic<float>[size];
        for (size_t i = 0; i < size; ++i)
            _buffer[i].store(0.0f, std::memory_order_relaxed);
    }
    ~LockFreeRingBuffer() { delete[] _buffer; }
    LockFreeRingBuffer(const LockFreeRingBuffer &) = delete;
    LockFreeRingBuffer &operator=(const LockFreeRingBuffer &) = delete;

    void push(float value)
    {
        const auto t = _tail.load(std::memory_order_relaxed);
        _buffer[t].store(value, std::memory_order_relaxed);
        size_t next = t + 1;
        if (next == _size)
            next = 0;
        _tail.store(next, std::memory_order_release);
        // Перезапись старых данных допустима — нам нужен последний интервал
    }
    bool pop(float &value)
    {
        const auto h = _head.load(std::memory_order_relaxed);
        if (h == _tail.load(std::memory_order_acquire))
            return false;
        value = _buffer[h].load(std::memory_order_relaxed);
        size_t next = h + 1;
        if (next == _size)
            next = 0;
        _head.store(next, std::memory_order_release);
        return true;
    }
    size_t capacity() const { return _size; }

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
    // std::atomic<bool> pleaseStop{false};

    RecorderModule(std::string name) : folderSelect("%ROOT%/recordings"), folderSelect_akf("%ROOT%/recordings")
    {
        flog::info("start constructor RecorderModule");
        this->name = name;
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_recorderInstances.push_back(this);

        root = (std::string)core::args["root"];
        strcpy(nameTemplate, "$f_$h-$m-$s_$d-$M-$y");
        // folderSelect_akf = folderSelect;
        // Define option lists
        containers.define("WAV", wav::FORMAT_WAV);
        // containers.define("RF64", wav::FORMAT_RF64); // Disabled for now
        // Define option lists
        // https://github.com/qrp73/SDRPP/blob/master/misc_modules/recorder/src/main.cpp
        // containers.define("FLAC", wav::FORMAT_FLAC);
        // containers.define("MP3", wav::FORMAT_MP3);

        sampleTypes.define(wav::SAMP_TYPE_UINT8, "Uint8", wav::SAMP_TYPE_UINT8);
        sampleTypes.define(wav::SAMP_TYPE_INT16, "Int16", wav::SAMP_TYPE_INT16);
        sampleTypes.define(wav::SAMP_TYPE_INT32, "Int32", wav::SAMP_TYPE_INT32);
        sampleTypes.define(wav::SAMP_TYPE_FLOAT32, "Float32", wav::SAMP_TYPE_FLOAT32);
        sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16); // SAMP_TYPE_UINT8);

        samplingRates.define(SAMP_TYPE_8k, "8000", SAMP_TYPE_8k);
        samplingRates.define(SAMP_TYPE_11k025, "11025", SAMP_TYPE_11k025);
        samplingRates.define(SAMP_TYPE_16k, "16000", SAMP_TYPE_16k);
        samplingRates.define(SAMP_TYPE_44к1, "44100", SAMP_TYPE_44к1);
        samplingRates.define(SAMP_TYPE_48k, "48000", SAMP_TYPE_48k);
        samplingRateId = samplingRates.valueId(SAMP_TYPE_8k);

        containerId = containers.valueId(wav::FORMAT_WAV);
        // Load config
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
                return; // Выход из текущей обработки, чтобы не продолжать с некорректным путём
            }
        }
        akfUdpPort = SIport + NUM_INST; // добавь поле int akfUdpPort{0}; в классе
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
        // ignoreSilence = false;

        // flog::info("nameTemplate: {0}", nameTemplate);
        if (config.conf[name].contains("nameTemplate"))
        {
            // flog::info("1 nameTemplate: {0}", nameTemplate);
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
        if (preRecordTimeMs > 2000)
            preRecordTimeMs = 2000;
        if (name == "Запис")
        {
            preRecordTimeMs = 0;
        }
        if (update_conf)
            config.release(true);
        else
            config.release();

        flog::info("staring 0 constructor RecorderModule");
        // Init audio path
        volume.init(NULL, audioVolume, false);
        splitter.init(&volume.out);
        splitter.bindStream(&meterStream);
        meter.init(&meterStream);
        s2m.init(&stereoStream);

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this);
        if (!isPreRecordChannel(name))
        { /// Оставляем так как есть.
            monoSink.init(&s2m.out, monoHandler, this);
        }
        else
        { // только для Control
            monoSink.init(&s2m.out, preRecordMonoHandler, this);
        }
        // флаг для управления meter на время записи
        meterDetachedForRecord = false;

        thisInstance = thisInstance + "-1";

        if (!initShutdownPipe(shutdownPipeFd))
        {
            flog::error("Failed to create shutdown pipe for '%s': %s",
                        name.c_str(), strerror(errno));
        }
        else
        {
            flog::info("Shutdown pipe created: rfd=%d, wfd=%d", shutdownPipeFd[0], shutdownPipeFd[1]);
        }
        processing.store(0);
        gui::menu.registerEntry(name, menuHandler, this);

        flog::info("finish constructor RecorderModule");

        flog::warn(" RegisterInterface: {0}", name);
        core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler, this);
        // flog::warn("radio. 2. strm_name {0}, selectedStreamName {1}, name.size() {2}", strm_name.c_str(), selectedStreamName, name.size());
    }

    ~RecorderModule()
    {
        // flog::info("DESTRUCTOR for RecorderModule '{0}' ENTERED.", name);

        // 1) ЕДИНАЯ ТОЧКА ОСТАНОВКИ
        // stop(true) внутри:
        //  - write(shutdownPipeFd[1], 'x');  // будим select/recv
        //  - корректно останавливает DSP/файлы, снимает бинды
        //  - join analysisThread (без удержания analysisThreadMtx)
        //  - сбрасывает флаги recording/this_record/restart_pending/akfState
        is_destructing.store(true);
        try
        {
            flog::info("stop(true, true);", name);
            stop(true, true);
        }
        catch (...)
        {
            flog::error("DESTRUCTOR '{0}': stop(true) threw; continuing cleanup.", name);
        }

        // 2) Завершаем workerInfoThread
        // flog::info("DESTRUCTOR for '{0}': Awaiting workerInfoThread.join()...", name);
        if (workerInfoThread.joinable())
            workerInfoThread.join();
        if (analysisThread.joinable())
            analysisThread.join();
        // flog::info("DESTRUCTOR for '{0}': workerInfoThread joined.", name);

        // 4) Теперь можно безопасно закрывать пайпы
        closeShutdownPipe(shutdownPipeFd);

        // 5) Убираем интерфейсы/GUI
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);

        // 6) Снимаем подписки и локальные ресурсы
        deselectStream(); // быстро
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        meter.stop();
        if (writer_akf)
        {
            if (writer_akf->isOpen())
            {
                writer_akf->close();
            }            
            delete writer_akf;
            writer_akf = nullptr;
        }

        // 7) Удаляем себя из глобального списка (потоки уже мертвы)
        {
            std::lock_guard<std::mutex> lock(g_instancesMutex);
            g_recorderInstances.erase(
                std::remove(g_recorderInstances.begin(), g_recorderInstances.end(), this),
                g_recorderInstances.end());
        }

        // flog::info("DESTRUCTOR for RecorderModule '{0}' FINISHED.", name);
    }

    std::string genRawFileName(int mode, std::string name)
    {
        std::string templ = "$t_$f_$h-$m-$s_$d-$M-$y.raw"; // "$y$M$d-$u-$f-$b-$n-$e.wav";
        // Get data
        time_t now = time(0);
        tm *ltm = localtime(&now);
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end())
        {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        // Select the record type string
        std::string type = (recMode == RECORDER_MODE_AUDIO) ? "audio" : "iq";

        // Format to string
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
            int mode = gui::mainWindow.getselectedDemodID(); //-1;
            // core::modComManager.callInterface(name, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
            if (mode >= 0)
            {
                modeStr = radioModeToString[mode];
            };
        }

        // Replace in template
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
        // {yymmdd}-{uxtime_ms}-{freq}-{band}-{receivername}.wav
        std::string templ = "$y$M$d-$u-$f-$b-$n-$m.wav"; // "$y$M$d-$u-$f-$b-$n-$e.wav";

        // Get data
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

        // Format to string
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
        // 230615-1686831173_250-107400000-300-rp10.wav
        // Replace in template
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
        // Bind stream register/unregister handlers
        flog::info("starting 1 postInit RecorderModule");
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
            currSource = sourcemenu::getCurrSource(); //  sigpath::sourceManager::getCurrSource();
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
        // flog::warn("radio. 6. selectedStreamName {0}, name. {1}", selectedStreamName, name);
        // Select the stream
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

        while (!g_stop_workers.load() && !_this->is_destructing.load()) //  && !_this->pleaseStop.load()
        {
            // рестарт
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

            AkfState currentState = _this->akfState.load();

            if (currentState == AkfState::AWAITING_PRERECORD_RESTART)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            else    
                std::this_thread::sleep_for(std::chrono::milliseconds(250));

            if (_this->is_destructing.load())
                break; // ранний выход

            if (!_this->isControl)
            {
                /// std::lock_guard<std::mutex> lock(g_instancesMutex);
                if (_this->isServer && _this->name == "Запис")
                {
                    if (gui::mainWindow.getUpdateMenuRcv3Record())
                    {
                        // std::lock_guard<std::recursive_mutex> lck(_this->recMtx);
                        bool should_record_from_gui = gui::mainWindow.getServerRecording(0); // getRecording();
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
                                _this->processing.store(1);
                            }
                        }
                        else
                        {
                            if (_this->recording.load())
                            {
                                gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                                _this->stop(false, false);
                                gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                                _this->processing.store(0);
                            }
                        }

                        gui::mainWindow.setUpdateMenuRcv3Record(false);
                        continue;
                    }
                }
            }

            // ===== фон: АКФ + рестарт =====
            bool restart_needed = _this->_restart.load();

            if (currentState == AkfState::ANALYSIS_PENDING ||
                currentState == AkfState::NOISE_DETECTED ||
                currentState == AkfState::SIGNAL_DETECTED ||
                currentState == AkfState::AWAITING_PRERECORD_RESTART ||
                restart_needed)
            {
                // std::lock_guard<std::recursive_mutex> lck(_this->recMtx);
                currentState = _this->akfState.load();

                if (currentState == AkfState::ANALYSIS_PENDING)
                {
                    // ВАЖНО: дергаем stop_akf(true) ТОЛЬКО если файл ещё пишется
                    if (_this->writer_akf)
                    {
                        flog::info("[WorkerInfo] '{0}': ANALYSIS_PENDING -> moving AKF file once.", _this->name);
                        _this->stop_akf(true); // внутри writer_akf станет nullptr
                    }
                    // Если writer_akf уже nullptr — не логируем и не делаем ничего,
                    // ждём UDP‑результат (моно‑хэндлер увидит его и сменит состояние)
                }
                else if (currentState == AkfState::AWAITING_PRERECORD_RESTART) // Обработчик отложенного перезапуска предзаписи
                {
                    // Проверяем, что модуль не уничтожается и не начал новую запись извне
                    if (!_this->is_destructing.load() && !_this->recording.load())
                    {
                        flog::info("[WorkerInfo] Executing delayed pre-record restart for '{0}'", _this->name);
                        _this->startAudioPath();
                    }
                    else
                    {
                        flog::info("[WorkerInfo] Pre-record restart for '{0}' was skipped.", _this->name);
                    }
                    // Возвращаемся в исходное состояние
                    _this->akfState.store(AkfState::IDLE);
                }
                else if (currentState == AkfState::NOISE_DETECTED)
                {
                    flog::info("[WorkerInfo] '{0}': NOISE_DETECTED. Performing safe stop.", _this->name);
                    if (_this->writer_akf)
                    {
                        flog::info("[WorkerInfo] '{0}': ANALYSIS_PENDING -> moving AKF file once.", _this->name);
                        _this->stop_akf(true); // внутри writer_akf станет nullptr
                    }

                    if (_this->recording.load())
                    {
                        // 1. Безопасный останов
                        _this->stop(false, false);

                        // 2. Сообщение Supervisor'у
                        _this->processing.store(0);

                        // 3. Переводим в состояние ожидания перезапуска, а не ставим флаг
                        // _this->akfState.store(AkfState::AWAITING_PRERECORD_RESTART);

                        if (!_this->_restart.load())
                        {
                            gui::mainWindow.setRecording(_this->recording.load());
                            gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                        }
                    }
                    else
                    {
                        // Если записи не было, просто возвращаемся в IDLE
                        _this->akfState.store(AkfState::IDLE);
                    }

                    if (std::filesystem::exists(_this->curr_expandedPath_akf))
                        std::filesystem::remove(_this->curr_expandedPath_akf);
                }
                else if (currentState == AkfState::SIGNAL_DETECTED)
                {
                    flog::info("[WorkerInfo] '{0}': SIGNAL_DETECTED. Finalizing recording.", _this->name);
                    // Гарантированно очищаем writer_akf, если он еще существует
                    if (_this->writer_akf) {
                        flog::info("[WorkerInfo] '{0}': SIGNAL_DETECTED -> performing defensive cleanup of AKF writer.", _this->name);
                        _this->stop_akf(false); // Вызываем с 'false', чтобы просто удалить временный файл
                    }                    
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

    //=====================================================
    void start()
    {
        // Флаг, который определит, нужно ли запускать поток анализа ПОСЛЕ снятия блокировки
        bool should_start_analysis_thread = false;

        if (name == "Запис")
            gui::mainWindow.setUpdateMenuRcv3Record(false);
        // Используем блок для ограничения области видимости и времени жизни lock_guard
        {
            flog::info("Starting start()");
            std::unique_lock<std::recursive_mutex> lck(recMtx);
            if (recording.load())
            {
                return;
            }
            flog::info("Starting start() 1");

            // =================================================================
            // ШАГ 1: ОБЩАЯ ПОДГОТОВКА И ИНИЦИАЛИЗАЦИЯ
            // =================================================================

            // Сброс состояний от предыдущей записи
            analysisResultSignal.store(ANALYSIS_PENDING);
            Signal = -1;
            initialized = false;
            rms_history.clear();
            insert_REC.store(false);

            // Чтение конфигурации
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
            akfUdpPort = SIport + NUM_INST; // добавь поле int akfUdpPort{0}; в классе
            flog::info("[RECORDER START {0}] Map: NUM_INST={1}, shortRecDirectory={2}, akfUdpPort={3}", name.c_str(), NUM_INST, shortRecDirectory.c_str(), akfUdpPort);
            // Генерация имени файла, если его нет
            // ===== 1) Сначала собираем данные ДЛЯ ИМЕНИ ФАЙЛА БЕЗ recMtx =====
            if (currWavFile.empty())
            {
                int _mode = gui::mainWindow.getselectedDemodID();
                std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "Канал приймання";
                /*
                if (gui::waterfall.selectedVFO != "" && core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio")
                {
                ///    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                }
                */
                flog::info("calc current freq...");
                current = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[vfoName]->generalOffset;
                flog::info("current freq ready: %.0f", current);
                currWavFile = (recMode == RECORDER_MODE_AUDIO) ? expandString(genWavFileName(current, _mode)) : genRawFileName(recMode, vfoName);
            }
            flog::info("Starting recording for file: {0}", currWavFile);

            // Настройка writer'а
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
            writer.setChannels((recMode == RECORDER_MODE_AUDIO && !stereo) ? 1 : 2);
            writer.setSampleType(sampleTypes[sampleTypeId]);
            writer.setSamplerate(samplerate);

            std::string tmp_dir = wavPath + "/../tmp_recording"; // Общая временная директория
            makeDir(wavPath.c_str());                            // Убеждаемся, что конечная папка для wav существует
            makeDir(tmp_dir.c_str());
            expandedPath = expandString(tmp_dir + "/" + currWavFile);
            curr_expandedPath = expandString(wavPath + "/" + currWavFile);
            // =================================================================
            // ШАГ 2: ОТКРЫТИЕ ОСНОВНОГО ФАЙЛА ЗАПИСИ
            // =================================================================
            flog::info("Opening main recording file: {0}", expandedPath);
            if (!writer.open(expandedPath))
            {
                flog::error("Failed to open main file for recording: {0}. Aborting start.", expandedPath);
                return;
            }

            // =================================================================
            // ШАГ 3: ВЫБОР СЦЕНАРИЯ (АКФ ВКЛЮЧЕН ИЛИ ВЫКЛЮЧЕН)
            // =================================================================
            flog::info("[RECORDER {0}] Entering start(). Current state: {1}, recMode: {2}",
                       name.c_str(), akfStateToString(akfState.load()), recMode);
            const bool use_akf = false; // wantAKF();

            if (use_akf)
            {
                if (!akfPreflightOk())
                {
                    // 💡 мягкий откат: делаем обычную запись без АКФ
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

                // директории уже создавались в префлайте; вызываем повторно — это idempotent
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
                // ====== Ветка без АКФ ======
                flog::info("[RECORDER {0}] AKF is DISABLED for this session (state={1}, flag_akf={2}).",
                           name.c_str(), akfStateToString(akfState.load()), flag_akf);
                initiateSuccessfulRecording(1);
            }

            // =================================================================
            // ШАГ 4: ЗАПУСК АУДИО-ПОТОКОВ (SINK'ОВ)
            // =================================================================
            if (recMode == RECORDER_MODE_AUDIO)
            {
                if (stereo)
                {
                    stereoSink.setInput(&stereoStream);
                    stereoSink.start();
                }
                else
                {
                    if (isPreRecord)
                    {
                        flog::info("Pre-record is active for {0}. Preparing for seamless start.", name);

                        // 1. Сбрасываем флаг. Теперь monoHandler знает, что при первом вызове нужно слить буфер.
                        preBufferDrained.store(false);

                        // 2. ПЕРЕКЛЮЧАЕМ обработчик с preRecordMonoHandler на основной monoHandler.
                        // Это ключевой шаг, который остается здесь.
                        s2m.stop();
                        monoSink.stop();
                        monoSink.init(&s2m.out, monoHandler, this);
                    }
                    else // Это ветка для канала "Запис" или если предзапись отключена
                    {
                        monoSink.stop();
                        monoSink.init(&s2m.out, monoHandler, this);
                    }
                    s2m.start();
                    monoSink.start();
                }
                splitter.bindStream(&stereoStream);
                isSplitterBound = true;
            }
            else if (recMode == RECORDER_MODE_PUREIQ)
            {
                stereoSink.start();
                splitter.bindStream(&stereoStream);
                dummySink.init(&stereoStream, dummyHandler, this);
                dummySink.start();
            }
            else
            {
                // flog::info("[RECORDER '{0}'] Entering start() in BASEBAND mode", name); // <-- НОВЫЙ ЛОГ
                basebandStream = new dsp::stream<dsp::complex_t>();
                basebandSink.setInput(basebandStream);
                basebandSink.start();
                // flog::info("[RECORDER '{0}'] Attempting to bind to iqFrontEnd...", name); // <-- НОВЫЙ ЛОГ
                sigpath::iqFrontEnd.bindIQStream(basebandStream);
                // flog::info("[RECORDER '{0}'] Successfully bound to iqFrontEnd", name); // <-- НОВЫЙ ЛОГ
            }

            // Финальные установки состояния
            recording.store(true);
            _restart.store(false);
            // if (gui::mainWindow.getServerStatus(0) == 0)
            gui::mainWindow.setRecording(recording.load());
            // gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
            flog::info("Starting 1 ...");

        } // Мьютекс recMtx здесь автоматически освобождается
        flog::info("Starting 2 ...");
        // =================================================================
        // ШАГ 5: ЗАПУСК ПОТОКА АНАЛИЗА (ЕСЛИ НУЖНО)
        // =================================================================
        if (should_start_analysis_thread)
        {
            flog::info("Starting AKF analysis thread...");
            runAnalysisTask(); // Эта функция запускает detach-поток
        }
        flog::info("Starting 3 ...");
    }

    // Возвращаем эту функцию в том виде, в котором она была
    // и который работал для перемещения файла.
    void stop_akf(bool flag_rename = true)
    {
        flog::info("[RECORDER {0}] [STOP_AKF] 1. Called with flag_rename: {1}",
                   name.c_str(), flag_rename);

        // Блокировка обязательна, так как мы работаем с общим ресурсом
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
            // Перемещаем файл для анализатора
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
            // Удаляем временный файл, если он оказался шумом
            flog::info("[RECORDER {0}] [STOP_AKF] 4b. Deleting temporary AKF file: {1}",
                       name.c_str(), expandedPath_akf);
            std::filesystem::remove(expandedPath_akf);
        }

        // Освобождаем память в любом случае
        delete writer_akf;
        writer_akf = nullptr;

        flog::info("[RECORDER {0}] [STOP_AKF] 6. Cleanup complete.", name.c_str());
    }

    void stop(bool rename_and_save = true, bool finish = false)
    {
        // 0) Один поток на стоп
        static_assert(std::atomic<bool>::is_always_lock_free, "atomic<bool> required");
        if (isStopping.exchange(true))
        {
            flog::warn("[RECORDER STOP] Already stopping '{0}'.", name);
            return;
        }

        flog::info("[RECORDER STOP] Cleanup sequence started for '{0}'...", name);

        // 1) Сигналим потокам
        pleaseStopAnalysis.store(true);
        restart_pending.store(false); // важный сброс, чтобы после stop() ничего не перезапустилось
        wakeShutdownPipe(shutdownPipeFd[1]);
        // 2) Корректно останавливаем анализ (если он не detach, см. п.3)
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lk(analysisThreadMtx);
            if (analysisThread.joinable())
                to_join = std::move(analysisThread);
        }
        if (to_join.joinable())
            to_join.join();
        /*
        if (analysisThread.joinable())
        {
            if (shutdownPipeFd[1] != -1)
            {
                char buf = 'x';
                // (void)write(shutdownPipeFd[1], &buf, 1);
                if (write(shutdownPipeFd[1], &buf, 1) == -1 && errno != EPIPE)
                {
                }
            }
            analysisThread.join();
        }
        */

        // 3) Останавливаем DSP и файл **только если реально шла запись**
        if (recording.load())
        {
            flog::info("[RECORDER] stop(): Stopping streams and closing writer...");

            if (recMode == RECORDER_MODE_AUDIO)
            {
                if (isSplitterBound)
                {
                    splitter.unbindStream(&stereoStream);
                    isSplitterBound = false;
                }
                monoSink.stop();
                stereoSink.stop();
                s2m.stop();
                //     flog::info("[RECORDER] processing {0}", processing);
                /*
                if (isPreRecordChannel(name) && !finish) //  && processing // isPreRecordChannel(name)
                {
                    monoSink.init(&s2m.out, preRecordMonoHandler, this);
                    // Перед стартом цепочки — подготовить размер буфера предзаписи
                    updatePreBufferSize();
                    startAudioPath();
                    // s2m.stop();
                    // monoSink.stop();
                    // monoSink.init(&s2m.out, preRecordMonoHandler, this);
                    // s2m.start();
                    // monoSink.start();
                }
                */
            }
            else if (recMode == RECORDER_MODE_PUREIQ)
            {
                splitter.unbindStream(&stereoStream);
                stereoSink.stop();
                dummySink.stop();
            }
            else
            { // baseband IQ
                if (basebandStream)
                {
                    sigpath::iqFrontEnd.unbindIQStream(basebandStream);
                    basebandSink.stop();
                    delete basebandStream;
                    basebandStream = nullptr;
                }
            }

            // Закрыть основной writer
            writer.close();

            // Файловые операции
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
                            // ... генерация .conf при необходимости ...
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

            // HTTP‑хвосты
            if (status_direction && this_record)
            {
                curlPOST_end_new();
                status_direction = false;
            }
        }
        else
        {
            if (isSplitterBound)
            {
                splitter.unbindStream(&stereoStream);
                isSplitterBound = false;
            }
            monoSink.stop();
            stereoSink.stop();
            s2m.stop();

            flog::info("[RECORDER] stop(): Already not recording. Skipping DSP/file close.");
        }

        // 4) Универсальный сброс состояния (всегда)
        recording.store(false);
        processing.store(0);
        this_record = false;
        currWavFile.clear();
        preBufferDrained.store(false);

        if (!finish && isPreRecordChannel(name))
        {
            // Если это не финальный останов и канал требует предзаписи,
            // планируем отложенный перезапуск.
            akfState.store(AkfState::AWAITING_PRERECORD_RESTART);
        }
        else
        {
            // Во всех остальных случаях — просто переходим в IDLE.
            akfState.store(AkfState::IDLE);
        }        
        analysisResultSignal.store(ANALYSIS_NONE);
        flog::info("[RECORDER] LEAVING stop() successfully.");
        isStopping.store(false);
        // if (gui::mainWindow.getServerStatus(0) == 0)
        gui::mainWindow.setRecording(recording.load());
        // gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
        flog::info("[RECORDER] Stop requested for '{0}'. Flag cleared.", name);
    }

private:
    std::atomic<AkfState> akfState{AkfState::IDLE};
    std::atomic<bool> restart_pending{false};
    std::mutex stopMtx;
    std::atomic<bool> isStopping{false};

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
        return 0; // "Запис"
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
            return va_root_dir; // недопустимый номер — возвращаем как есть
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

        // Если не соответствует шаблону — оставляем без изменений
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

        // Проверка на наличие ключей
        if (!parsed.contains("id") || !parsed.contains("filename"))
        {
            throw std::runtime_error("JSON does not contain required fields: 'id' and 'filename'\nRaw output:\n" + result);
        }

        // Проверка на валидные значения
        if (parsed.value("id", 0) == 0 || parsed.value("filename", "").empty())
        {
            throw std::runtime_error("JSON contains invalid values (id == 0 or empty filename)\nRaw output:\n" + result);
        }

        return parsed;
    }

    void restart()
    {
        flog::info("[RECORDER] Restart requested for '{0}'.", name);
        // 1. Устанавливаем флаг, что после остановки нужно будет сделать рестарт.
        restart_pending.store(true);

        // 2. Инициируем асинхронную остановку.
        stop(true, true); // true, так как при рестарте мы обычно хотим сохранить старый файл
        /*
        if (recording)
        {
            stop();
        }
        currWavFile = "";
        // start_short.store(false);
        start();
        */
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
        else
        {
        }

        _this->listener->acceptAsync(clientHandler, _this);
    }

    dsp::tap<float> generateLowpassTaps(float sampleRate, float cutoffFreq, int numTaps)
    {
        dsp::tap<float> taps = dsp::taps::alloc<float>(numTaps); // выделяем память

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

    // Функция исправления строки
    std::string fixMissingQuotes(const std::string &raw)
    {
        std::string fixed = raw;
        // Добавляем кавычки вокруг ключей (например, Signal -> "Signal")
        fixed = std::regex_replace(fixed, std::regex(R"(\b([A-Za-z_][A-Za-z0-9_]*)\b(?=\s*:))"), R"("$1")");
        // Добавляем кавычки к строковым значениям (например, DMR -> "DMR")
        fixed = std::regex_replace(fixed, std::regex(R"(:\s*([A-Za-z_][A-Za-z0-9_]*))"), R"(: "$1")");
        // Удаляем лишние пробелы или переносы строк
        fixed.erase(std::remove(fixed.begin(), fixed.end(), '\n'), fixed.end());
        fixed.erase(std::remove(fixed.begin(), fixed.end(), '\r'), fixed.end());
        return fixed;
    }
    // Функция для преобразования строки в JSON
    json parseUdpData(const std::string &info)
    {
        try
        {
            // Попытка парсинга строки в JSON
            json import = json::parse(info);
            return import;
        }
        catch (const json::parse_error &e)
        {
            // Обработка ошибок парсинга
            flog::warn("Ошибка парсинга JSON: {0}", e.what());
        }
        // if error
        std::string raw_data = info;
        std::string fixed_data = fixMissingQuotes(raw_data);
        try
        {
            // Попытка парсинга строки в JSON
            // info_withQuotes = "\"" + info_withQuotes + "\"";
            if (!fixed_data.empty() && fixed_data[0] == '{' && fixed_data.back() == '}')
            {
                // Преобразование строки в JSON
                json parsed_data = json::parse(fixed_data);
                // Вывод JSON
                std::cout << "Парсинг успешен:\n"
                          << parsed_data.dump(4) << std::endl;
                return parsed_data;
            }
            else
            {
                std::cerr << "Данные не являются корректным JSON.\n";
            }
        }
        catch (const json::parse_error &e)
        {
            std::cerr << "Ошибка парсинга JSON: " << e.what() << std::endl;
        }
        catch (const json::type_error &e)
        {
            std::cerr << "Ошибка типа JSON: " << e.what() << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Другая ошибка: " << e.what() << std::endl;
        }
        return nullptr; // Можно вернуть пустой объект или обработать иначе
    }

    bool makeDir(const char *dir)
    {
        if (fs::exists(dir))
            return fs::is_directory(fs::status(dir));
        else
            return fs::create_directories(dir);
    }

    static void menuHandler(void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        /*
        if (gui::mainWindow.getStopMenuUI())
        {
            return;
        }
        */

        float menuWidth = ImGui::GetContentRegionAvail().x;
        // Recording mode
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
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_FLOAT32); // ::SAMP_TYPE_INT16);
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
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_INT16); // SAMP_TYPE_UINT8);
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
        // Recording path
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
            // Show additional audio options
            if (!_this->Admin)
                ImGui::BeginDisabled();

            ImGui::LeftLabel("Цифровий потік");
            ImGui::FillWidth();
            int old_streamId = _this->streamId;
            if (ImGui::Combo(CONCAT("##_recorder_stream_", _this->name), &_this->streamId, _this->audioStreams.txt))
            {
                // std::string strm_name = _this->audioStreams.value(_this->streamId);
                // flog::info("TRACE! RECORD! name {0}, streamId {1}, new strm_name {2}", _this->name, _this->streamId, strm_name);
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
                    // flog::warn("TRACE! RECORD! 1 selectStream {0}", _this->strm_name);
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
                    // flog::info("TRACE! RECORD! strm_name .{0}., _this->selectedStreamName  {1}", strm_name, _this->selectedStreamName);

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
            // ImGui::FillWidth();
            // ImGui::VolumeMeter(_this->audioLvl.r, _this->audioLvl.r, -60, 10);
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

        // Record button
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
                    // _this->cntDATA = 0;
                    _this->akfState.store(AkfState::IDLE);
                    _this->currWavFile = "";
                    _this->this_record = true;
                    _this->start();
                    _this->processing.store(1);
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
                            _this->processing.store(1);
                            // if (gui::mainWindow.getServerStatus(0) > 0)
                            gui::mainWindow.setRecording(_this->recording.load());
                            // gui::mainWindow.setUpdateMenuSnd3Record(true);
                            // gui::mainWindow.setServerRecordingStart(gui::mainWindow.getCurrServer());
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
                    _this->stop(false, false);
                    _this->this_record = false;
                    // if (gui::mainWindow.getServerStatus(0) > 0)
                    gui::mainWindow.setRecording(_this->recording.load());
                    gui::mainWindow.setServerRecordingStop(gui::mainWindow.getCurrServer());
                    _this->processing.store(0);
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
                    //_this->cntDATA = 0;
                    _this->akfState.store(AkfState::IDLE);
                    _this->this_record = true;
                    _this->start();
                    gui::mainWindow.setServerRecordingStart(0);
                    _this->processing.store(1);
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
                            // if (gui::mainWindow.getServerStatus(0) > 0)
                            gui::mainWindow.setUpdateMenuSnd3Record(true);
                        }
                    }
                }
                if (_ifStartElseBtn)
                {
                    ImGui::EndDisabled();
                }
                // ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Очікування --:--:--");
            }
            else
            {
                if (ImGui::Button(CONCAT("ЗУПИНИТИ##_recorder_rec_stop", _this->name), ImVec2(menuWidth, 0)))
                {
                    _this->stop(false, false);
                    gui::mainWindow.setServerRecordingStop(0);
                    _this->processing.store(0);
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
                // bool rec = gui::mainWindow.getServerRecording(CHNL);
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

                    // ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Очікування --:--:--");
                }
                else
                {
                    if (ImGui::Button(CONCAT("ЗУПИНИТИ##_recorder_srv_stop", _this->name), ImVec2(menuWidth, 0)))
                    {
                        gui::mainWindow.setRecording(false);
                        gui::mainWindow.setServerRecordingStop(currSrvr);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrvr, true);
                        gui::mainWindow.setUpdateMenuSnd3Record(true);
                        _this->processing.store(0);
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

        int vaport = akfUdpPort; // SIport + NUM_INST;
        flog::info("[AKF WAIT] inst='{0}' port={1} file='{2}' dir='{3}'", name.c_str(), akfUdpPort, currWavFile.c_str(), shortRecDirectory.c_str());
        // Вызываем listenUDP с передачей пайпа
        if (akfUdpPort <= 0)
        {
            flog::error("[AKF WAIT {0}] Invalid AKF UDP port: {1}. Disabling AKF for this take.",
                        name.c_str(), akfUdpPort);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
            // не забываем state-машину: workerInfo/monoHandler доберут это и остановят запись как шум/таймаут
            return;
        }

        std::string info = net::listenUDP(host, vaport, akf_timeout_sec, interrupt_fd); // shutdownPipeFd[0]);
        akf_timeout_sec = 0;
        // ================== НАЧАЛО ИСПРАВЛЕНИЯ ==================
        // Теперь мы правильно обрабатываем ВСЕ возможные результаты от listenUDP

        if (info == "interrupted")
        {
            flog::info("Analysis for '{0}' was interrupted by a stop signal.", name);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        // if (ans == "error_bind" || ans == "error_select" || ans == "error_timeout") {
        else if (info.rfind("error", 0) == 0) // Ловит "error_bind", "error_timeout", и т.д.
        {
            flog::error("listenUDP for '{0}' failed with: {1}", name, info);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        else if (info.empty()) // На случай, если recvfrom вернул 0
        {
            flog::warn("listenUDP for '{0}' returned empty data.", name);
            analysisResultSignal.store(ANALYSIS_ERROR_OR_TIMEOUT);
        }
        else // Если это не ошибка и не прерывание, значит, это данные JSON
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
        // =================== КОНЕЦ ИСПРАВЛЕНИЯ ====================
    }

    void runAnalysisTask()
    {
        // 0) Новый запуск анализа — фиксируем состояние и снимаем рестарт
        pleaseStopAnalysis.store(false);
        drainShutdownPipe(shutdownPipeFd[0]);
        analysisResultSignal.store(ANALYSIS_PENDING);
        restart_pending.store(false);
        akf_timeout_sec = 0;

        // 1) Подготовка: корректно гасим предыдущий поток и очищаем пайп
        std::thread oldThread;
        {
            std::lock_guard<std::mutex> lk(analysisThreadMtx);

            // Будим старый поток (если был) и даём ему шанс выйти из select/recv
            // pleaseStop.store(true);
            wakeShutdownPipe(shutdownPipeFd[1]);

            // Забираем поток наружу, чтобы join делать ВНЕ мьютекса
            if (analysisThread.joinable())
                oldThread = std::move(analysisThread);

            // Сброс на новый запуск
            analysisSocketFd.store(-1);

            // Осушаем читающую сторону, чтобы старые сигналы не сорвали новый select()
            drainShutdownPipe(shutdownPipeFd[0]);

            // Готовим флаг к новому запуску
            // pleaseStop.store(false);
        }

        if (oldThread.joinable())
            oldThread.join();

        // 2) Стартуем новый joinable‑поток анализа
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
            // Если используешь этапы — можно раскомментировать:
            // akfState.store(AKF_RUNNING);
            // analysisResultSignal.store(ANALYSIS_RUNNING);

            // Внутри startServer() ОБЯЗАТЕЛЬНО используем shutdownPipeFd[0] как interrupt_fd
            // и возвращаемся быстро при прерывании.
            // this->startServer();
            const int interrupt_fd = this->shutdownPipeFd[0];
            if (interrupt_fd < 0) {
                flog::warn("startServer: interrupt_fd is invalid (<0); proceeding without interrupt.");
            }            
            this->startServer(interrupt_fd);

            // Успешное завершение
            // analysisResultSignal.store(ANALYSIS_OK);
            // akfState.store(AKF_DONE);
            guard.finished_ok = true;
        }
        catch (const std::exception& e) {
            flog::error("An exception in analysis thread for '{0}': {1}", this->name, e.what());
            // Guard проставит ERROR
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
            updatePreBufferSize();

            startAudioPath();
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
        // flog::warn("--- TRACE! audioStream {0}", selectedStreamName);

        if (recording.load() && recMode == RECORDER_MODE_AUDIO)
        {
            stop(false, false);
            gui::mainWindow.setServerRecordingStop(0);
        }
        {
            std::lock_guard<std::recursive_mutex> lck(recMtx);
            stopAudioPath();
            sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        }
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath()
    {
        flog::warn("startAudioPath");

        volume.start();
        splitter.start();
        meter.start();

        if (isPreRecordChannel(name))
        {
            flog::warn("isPreRecordChannel({0})", name);

            // monoSink.stop();
            // s2m.stop();
            // monoSink.init(&s2m.out, preRecordMonoHandler, this);
            s2m.start();
            monoSink.start();
            splitter.bindStream(&stereoStream);
            // flog::warn("preRecordMonoHandler START()");
            audioPathRunning = true;
        }
    }

    void stopAudioPath()
    {
        volume.stop();
        splitter.stop();
        meter.stop();
    }

    static void streamRegisteredHandler(std::string name, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;

        // Add new stream to the list
        _this->audioStreams.define(name, name, name);

        // If no stream is selected, select new stream. If not, update the menu ID.
        if (_this->selectedStreamName.empty())
        {
            _this->selectStream(name);
            // flog::warn("TRACE! RECORD! 0 selectStream {0}", name);
        }
        else
        {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
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
        // Note: Yes, using the natural log is on purpose, it just gives a more beautiful result.
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

    // 🔹 Константы шумоподавления    ==========================================================================
    const float SILENCE_LVL = 10e-5;
    /*
    1e-5 (0.00001) – слабый порог, оставляет почти все звуки.
    1e-4 (0.0001) – средний порог, фильтрует слабый шум.
    1e-3 (0.001) – агрессивный порог, приглушает почти всю тишину.
    */
    const float NOISE_REDUCTION_FACTOR = 0.05f;
    // constexpr float NOISE_REDUCTION_FACTOR = 0.05f;  // Коэффициент понижения шума
    /*
    0.1 – шум заметен, но слабее
    0.05 – шум почти не слышен
    0.01 – шум максимально подавлен, но может стать полностью неслышным
    */
    const float NOISE_THRESHOLD = 0.2f * SILENCE_LVL;
    // const float SIGNAL_LVL = 0.19f;
    const float NOISE_LVL = 0.5f;
    const float NOISE_SUPPRESSION_EXPONENT = 0.5f;
    const int CALIBRATION_FRAMES = 1000; // Количество кадров для калибровки
    const float NOISE_LVL_INITIAL = 0.4f;
    const float SIGNAL_LVL_INITIAL = 0.19f;
    const float MIN_NOISE_LEVEL = 0.0005f;
    const int RMS_HISTORY_SIZE = 20;         // Размер истории значений RMS
    const float RMS_SMOOTHING_FACTOR = 0.1f; // Коэффициент сглаживания резких скачков

    // 🔧 Модифицированная функция адаптивного подавления шума
    inline float adaptiveNoiseReduction(float rms)
    {
        if (rms > NOISE_LVL)
            return MIN_NOISE_LEVEL; // Максимальное приглушение шума
        if (rms > signal_lvl)
        {
            float normalized = (NOISE_LVL - rms) / (NOISE_LVL - signal_lvl);
            // Защита от выхода за границы [0, 1]
            normalized = fmaxf(0.0f, fminf(normalized, 1.0f));
            // Теперь безопасно
            return MIN_NOISE_LEVEL + (1.0f - MIN_NOISE_LEVEL) * powf(normalized, NOISE_SUPPRESSION_EXPONENT);
            //    return MIN_NOISE_LEVEL + (1.0f - MIN_NOISE_LEVEL) *
            //                                 powf((noise_lvl - rms) / (noise_lvl - signal_lvl), NOISE_SUPPRESSION_EXPONENT);
        }
        return 1.0f; // Оставляем полезный сигнал без изменений
    }
    // 🔹 Дополнительные функции для тестирования различных степеней шумоподавления
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
        // АКФ работает только для аудио режима
        if (recMode != RECORDER_MODE_AUDIO)
            return false;

        // Глобальный флаг из конфига
        if (!flag_akf)
            return false;

        // Состояние устанавливается извне (GUI/модули)
        // RECORDING = "записать короткий фрагмент и ждать UDP"
        return akfState.load() == AkfState::RECORDING;
    }

    bool akfPreflightOk()
    {
        try
        {
            // 1) Папки (final + tmp)
            std::string akf_final_dir = shortRecDirectory;
            std::string akf_tmp_dir = shortRecDirectory + "/../tmp_recording";

            std::filesystem::create_directories(akf_final_dir);
            std::filesystem::create_directories(akf_tmp_dir);

            // 2) (опционально) лёгкая проверка bind'а на наш порт
            //   Если не хочешь трогать сеть — убери блок ниже.
            int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s < 0)
            {
                flog::warn("[RECORDER {0}] [AKF] preflight: socket() failed: {1}",
                           name.c_str(), strerror(errno));
            }
            else
            {
                int one = 1;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(akfUdpPort); // см. ниже где выставляем
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::bind(s, (sockaddr *)&addr, sizeof(addr)) != 0)
                {
                    int e = errno;
                    ::close(s);
                    // EADDRINUSE — порт занят кем-то (например, незавершённый процесс анализатора/другой инстанс)
                    flog::error("[RECORDER {0}] [AKF] preflight: UDP port {1} not free: {2}",
                                name.c_str(), akfUdpPort, strerror(e));
                    return false;
                }
                ::close(s);
            }
            return true;
        }
        catch (const std::exception &e)
        {
            flog::error("[RECORDER {0}] [AKF] preflight failed: {1}",
                        name.c_str(), e.what());
            return false;
        }
    }
    void deferredStop(bool flag_rename)
    {
        // Небольшая пауза, чтобы дать monoHandler гарантированно завершиться.
        // 10 миллисекунд более чем достаточно.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        this->stop_akf(false);
        // Вызываем нашу обычную функцию stop
        this->stop(flag_rename);
    }

    void processAkfSuccess(int signalValue)
    {
        // Небольшая пауза, чтобы monoHandler вышел из текущего вызова.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        flog::info("[PROCESSOR] Processing successful AKF result with signal {0}", signalValue);

        // Блокируем мьютекс, чтобы безопасно изменять состояние модуля
        std::lock_guard lck(recMtx);

        // Проверяем, не была ли запись уже остановлена по другой причине
        if (!recording.load())
        {
            flog::warn("[PROCESSOR] Recording was stopped before AKF success could be processed. Aborting.");
            return;
        }

        // Проверяем, не обработали ли мы уже этот сигнал
        if (insert_REC.load())
        {
            flog::warn("[PROCESSOR] AKF success has already been processed. Ignoring duplicate call.");
            return;
        }

        // Обновляем Signal для внешнего интерфейса
        Signal = signalValue;
        status_direction = true;

        // Устанавливаем флаг, что мы начали обработку
        insert_REC.store(true);

        // =============================================================
        // ВЫПОЛНЯЕМ КРИТИЧЕСКУЮ ОПЕРАЦИЮ
        // =============================================================
        // `insert_REC_PGDB` вернет true и обновит currWavFile (по ссылке или как-то еще)
        // Предположим, что она изменяет this->currWavFile
        if (!insert_REC_PGDB(this->currWavFile))
        {
            flog::error("[PROCESSOR] FAILED to create record in DB for {0}", this->currWavFile);
        }
        else
        {
            // Теперь, когда имя файла обновлено, используем его
            flog::info("[PROCESSOR] Record in DB for new file name '{0}' created successfully!", this->currWavFile);

            // Обновляем пути, которые могут понадобиться в stop()
            this->folderSelect.setPath(this->wavPath);
            this->curr_expandedPath = this->expandString(this->folderSelect.path + "/" + this->currWavFile);

            flog::info("[PROCESSOR] Starting POST request for {0}", this->currWavFile);
            if (this->this_record)
                this->curlPOST_begin_new();
        }
    }

    void initiateSuccessfulRecording(int signalValue)
    {
        // Эта функция может быть вызвана из разных потоков,
        // поэтому блокировка в самом начале обязательна.
        std::lock_guard lck(recMtx);

        // Проверяем, не была ли запись уже остановлена или обработана.
        if (insert_REC.load())
        {
            flog::warn("[INITIATOR] Recording already stopped or initiated. Aborting.");
            return;
        }

        flog::info("[INITIATOR] Initiating successful recording with signal {0}", signalValue);

        // Устанавливаем все флаги и переменные
        Signal = signalValue;
        status_direction = true;
        insert_REC.store(true);

        // Выполняем запись в БД и получаем новое имя файла
        if (!insert_REC_PGDB(this->currWavFile))
        {
            flog::error("[INITIATOR] FAILED to create record in DB for {0}", this->currWavFile);
        }
        else
        {
            // Успех! Обновляем пути с новым именем файла
            flog::info("[INITIATOR] Record in DB for new file name '{0}' created successfully!", this->currWavFile);
            this->folderSelect.setPath(this->wavPath);
            this->curr_expandedPath = this->expandString(this->folderSelect.path + "/" + this->currWavFile);

            flog::info("[INITIATOR] Starting POST request for {0}", this->currWavFile);
            if (this->this_record)
                this->curlPOST_begin_new();
        }
    }
    void updatePreBufferSize()
    {
        // Предзапись только в аудио-режиме
        if (recMode != RECORDER_MODE_AUDIO || stereo)
        {
            preBufferSizeInSamples = 0;
            monoPreBuffer.reset();
            return;
        }

        uint64_t sr = 0;
        if (!selectedStreamName.empty())
            sr = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);

        if (sr == 0)
        {
            monoPreBuffer.reset();
            preBufferSizeInSamples = 0;
            return;
        }

        size_t need = static_cast<size_t>((sr * preRecordTimeMs) / 1000.0);
        if (need < 1024)
            need = 1024;

        if (!monoPreBuffer || monoPreBuffer->capacity() != need)
        {
            monoPreBuffer = std::make_unique<LockFreeRingBuffer>(need);
            flog::info("Pre-record ring buffer allocated: {} samples ({} ms at {} Hz)",
                       need, preRecordTimeMs, sr);
        }
        preBufferSizeInSamples = need;
    }
    // --- отладка предбуфера: добавьте счётчик, чтобы убедиться, что handler идёт
    static void preRecordMonoHandler(float *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;

        // flog::warn("preRecordMonoHandler isPreRecord {0} ", _this->isPreRecord);
        if (!_this->isPreRecord)
            return;

        if (!data || count <= 0)
            return;

        // во время записи предбуфер не наполняем
        if (_this->recording.load())
            return;
        if (!_this->preRecord)
            return;

        auto *buf = _this->monoPreBuffer.get();
        if (!buf)
            return;

        // flog::warn("preRecordMonoHandler 2 count {0} ", count);
        for (int i = 0; i < count; ++i)
        {
            buf->push(data[i]);
        }
    }

    //=============================================================
    // 🔹 Основная функция обработки звука (моно)
    static void monoHandler(float *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (!data || count <= 0)
            return;

        // Проверяем, нужно ли сбросить буфер предзаписи.
        // Это делается только один раз за сессию.
        if (_this->isPreRecord && !_this->preBufferDrained.load())
        {
            bool expected = false;
            if (_this->preBufferDrained.compare_exchange_strong(expected, true))
            {
                std::vector<float> tempBuf;
                tempBuf.reserve(_this->preBufferSizeInSamples > 0 ? _this->preBufferSizeInSamples : 96000);
                float sample;
                while (_this->monoPreBuffer->pop(sample))
                {
                    tempBuf.push_back(sample);
                }

                if (!tempBuf.empty())
                {
                    flog::info("Writing {0} pre-recorded samples...", tempBuf.size());

                    // 1. Всегда пишем в основной файл `writer`, если он открыт.
                    if (_this->writer.isOpen())
                    {
                        _this->writer.write(tempBuf.data(), tempBuf.size());
                        flog::info("-> Wrote to main file.");
                    }

                    // 2. Если активен режим АКФ, дополнительно пишем в `writer_akf`.
                    if (_this->akfState.load() == AkfState::RECORDING)
                    {
                        if (_this->writer_akf && _this->writer_akf->isOpen())
                        {
                            _this->writer_akf->write(tempBuf.data(), tempBuf.size());
                            flog::info("-> Wrote to AKF file as well.");
                        }
                    }
                }
                // Сбрасываем флаг isPreRecord после использования буфера
                _this->isPreRecord = false;
            }
        }
        // =================================================================
        // ШАГ 1: ПРОВЕРКА РЕЗУЛЬТАТА АНАЛИЗА (если мы его ждем)
        // =================================================================
        // Эта проверка имеет высший приоритет, так как может привести к остановке.
        // Мы проверяем это, только если находимся в состоянии ожидания.
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

                // ✅ результат потреблён
                _this->analysisResultSignal.store(ANALYSIS_NONE);
            }
        }
        // =================================================================
        // ШАГ 2: ЗАПИСЬ АУДИОДАННЫХ
        // =================================================================

        // Запись в ОСНОВНОЙ файл происходит всегда, пока идет запись.
        _this->writer.write(data, count);
        // flog::info("            [MONO_HANDLER] count {0}", count);
        // Запись во ВРЕМЕННЫЙ файл АКФ происходит только в состоянии RECORDING.
        if (_this->akfState.load() == AkfState::RECORDING)
        {
            // Проверяем, что writer_akf существует и открыт
            if (_this->writer_akf && _this->writer_akf->isOpen())
            {
                _this->writer_akf->write(data, count);

                // Проверяем, не достигнута ли нужная длительность для анализа
                uint64_t samples_written_akf = _this->writer_akf->getSamplesWritten();
                if ((samples_written_akf / _this->samplerate) >= _this->maxRecShortDur_sec)
                {
                    // Короткая запись завершена. Переводим состояние в "ожидание анализа".
                    // workerInfo увидит это состояние и вызовет stop_akf(true).
                    _this->akfState.store(AkfState::ANALYSIS_PENDING);
                    flog::info("[MONO_HANDLER] Short recording phase finished. State -> ANALYSIS_PENDING.");
                }
            }
        }

        // =================================================================
        // ШАГ 3: ПРОВЕРКА НА МАКСИМАЛЬНУЮ ДЛИТЕЛЬНОСТЬ ЗАПИСИ
        // =================================================================
        // Эта логика не связана с АКФ и должна работать всегда.
        // Она атомарно выставляет флаг, на который также среагирует workerInfo.
        if (!_this->_restart.load())
        {
            uint64_t seconds_main = _this->writer.getSamplesWritten() / _this->samplerate;
            if (seconds_main >= _this->maxRecDuration * 60)
            {
                flog::info("[MONO_HANDLER] Max recording duration ({0}s) reached. Triggering restart.", seconds_main);
                _this->_restart.store(true); // Этот флаг будет обработан потоком workerInfo.
            }
        }
    }

    static void stereoHandler(dsp::stereo_t *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (_this->ignoreSilence)
        {
            float absMax = 0.0f;
            float *_data = (float *)data;
            int _count = count * 2;
            for (int i = 0; i < _count; i++)
            {
                float val = fabsf(_data[i]);
                if (val > absMax)
                {
                    absMax = val;
                }
            }
            _this->ignoringSilence = (absMax < _this->SILENCE_LVL);
            if (_this->ignoringSilence)
            {
                return;
            }
        }
        fprintf(stderr, "[recorder] stereoHandler called: count = %d\n", count);
        _this->writer.write((float *)data, count);
    }

    static void complexHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        // flog::info("complexHandler: called with count = {}", count);
        uint64_t seconds = _this->writer.getSamplesWritten() / _this->samplerate;
        if (_this->_restart == false)
        { // _this->radioMode > 0 &&
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
        // ничего не делаем
    }

    static void iqHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RecorderModule *_this = static_cast<RecorderModule *>(ctx);
        _this->writer.write(reinterpret_cast<float *>(data), count * 2); // 2 float per complex sample
    }

    static void moduleInterfaceHandler(int code, void *in, void *out, void *ctx)
    {
        RecorderModule *_this = (RecorderModule *)ctx;
        if (code == RECORDER_IFACE_CMD_STOP)
        {
            flog::info("RECORDER_IFACE_CMD_STOP: name {0}, _this->recording {1}", _this->name, _this->recording.load());
            // _this->stop_was_requested.store(true);
            _this->stop(true, true);
            _this->akfState.store(AkfState::IDLE);
            _this->processing.store(0);
            return;
        }
        std::lock_guard lck(_this->recMtx);

        if (code == RECORDER_IFACE_GET_SIGNAL)
        {
            int *_out = (int *)out;
            *_out = _this->Signal;
            // flog::info("[IFACE] GET_SIGNAL_AKF {0}", _this->Signal);
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
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_INT16); // SAMP_TYPE_UINT8);
            else
                _this->sampleTypeId = _this->sampleTypes.valueId(wav::SAMP_TYPE_FLOAT32); // ::SAMP_TYPE_INT16
            // sigpath::sinkManager.setStreamSampleRate(_this->selectedStreamName, diffSamplingRate);
        }
        else if (code == RECORDER_IFACE_CMD_START)
        {
            int *_in = (int *)in;
            std::string s((char *)_in);
            _this->currWavFile = _this->expandString(s);
            flog::info("RECORDER_IFACE_CMD_START: name {0}, recording {1},  file = {2}, _this->akfState {3}", _this->name, _this->recording.load(), _this->currWavFile, akfStateToString(_this->akfState.load()));
            // _this->start_short = false;
            //  sigpath::sinkManager.setStreamSampleRate(_this->selectedStreamName, diffSamplingRate);
            if (!_this->recording.load())
            {
                _this->start();
            }
        }
        else if (code == RECORDER_IFACE_GET_TEMPL)
        {
            std::string *_out = (std::string *)out;
            std::string currWavFile = std::string(_this->nameTemplate) + ".wav";
            *_out = currWavFile.c_str(); // ->nameTemplate;
        }
        // else if (code == RECORDER_IFACE_CMD_STOP)
        // {
        //     _this->akfState.store(AkfState::IDLE);
        //     _this->stop(true);
        // }
        else if (code == RECORDER_IFACE_CMD_SET_STREAM)
        {
            int *_in = (int *)in;
            // std::string _StreamName((char*)_in);
            /*
            config.acquire();
            if (config.conf[_this->name].contains("audioStream")) {
                _this->selectedStreamName = config.conf[_this->name]["audioStream"];
            }
            else {
                _this->selectedStreamName = "";
            }
            config.release();
            */
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
            // sigpath::iqFrontEnd.setSampleRate(8000);
            flog::info("RECORDER_IFACE_CMD_SET_STREAM _this->streamId {0}, strm_name {1}", _this->streamId, strm_name);
            _this->selectStream(_this->audioStreams.value(_this->streamId));
            // sigpath::sinkManager.setStreamSampleRate(_this->selectedStreamName, diffSamplingRate);
            // _this->streamId = 0;
        }
        else if (code == MAIN_SET_START)
        {
            flog::info("MAIN_SET_START processing {0}", _this->processing.load());
            _this->processing.store(1);
        }
        else if (code == MAIN_SET_STOP)
        {
            flog::info("MAIN_SET_STOP processing {0}", _this->processing.load());
            _this->processing.store(0);
        }
        else if (code == MAIN_GET_PROCESSING)
        {
            int *_out = (int *)out;
            // flog::info("RECORDER MAIN_GET_PROCESSING _this->processing {0}",_this->processing);
            *_out = _this->processing.load();
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
                // flog::info("MAIN_GET_STATUS_CHANGE {0}", _this->changing);
                *_out = 1;
                // flog::info("MAIN_GET_STATUS_CHANGE {0}", _this->changing);
                _this->changing = 0;
            }
            else
            {
                *_out = 0;
            }
        }
    }

    //=====================================================
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
        // Оставим только имя файла без пути и расширения
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
        // 20250525-1748195540333-500650000-12500-P1-4-ЧМ.wav

        std::string unixtime = parts[1];
        std::string freq = parts[2];
        std::string band = parts[3];

        // 🟡 receiver: берём только цифры из parts[4]
        int receiver_num = 0;
        for (char c : parts[4])
        {
            if (std::isdigit(c))
                receiver_num = receiver_num * 10 + (c - '0');
        }
        std::string receiver = std::to_string(receiver_num);

        std::string radiomode = parts[5];
        std::string modulation = parts[6];

        // 🟢 Создаём новое имя файла с заменой поля "receiver" на цифру
        std::string new_filename = parts[0] + "-" + unixtime + "-" + freq + "-" + band + "-" +
                                   receiver + "-" + radiomode + "-" + modulation + ".wav";
        // 🟢 Аргументы для скрипта
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
                // Быстрые таймауты
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 750L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 400L);
                // Отключить проверку сертификата для localhost (если нужен HTTPS)
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

    //=================================================
    enum
    {
        SINK_MODE_TCP,
        SINK_MODE_UDP
    };
    // Новая система флагов
    enum StopReason
    {
        NONE = 0,
        FROM_NOISE = 1, // Остановка из-за шума
        FROM_GUI = 2    // Обычная остановка пользователем (если нужно)
    };
    enum class AkfDecision
    {
        PENDING,  // Решение еще не принято
        CONTINUE, // Запись продолжается
        STOP      // Запись нужно остановить
    };
    std::atomic<AkfDecision> akfDecision;

    static constexpr int ANALYSIS_NONE = -2;
    static constexpr int ANALYSIS_PENDING = -1;         // Анализ еще не завершен
    static constexpr int ANALYSIS_NOISE_OR_TIMEOUT = 0; // Шум, ошибка или таймаут
    static constexpr int ANALYSIS_ERROR_OR_TIMEOUT = 3; // НВ

    // Атомарная переменная для результата.
    // Если > 0, это значение сигнала.
    // Если равно константе, это статус.
    std::atomic<int> analysisResultSignal{ANALYSIS_PENDING};
    //=================================================================================================================
    bool saveInDir = false;
    std::string wavPath = "/opt/recordings/";
    std::string expandedPath;
    std::string expandedPath_akf;
    std::string curr_expandedPath;
    std::string curr_expandedPath_akf;

    // std::string name;
    bool enabled = true;
    std::string root;
    char nameTemplate[1024];

    OptionList<std::string, wav::Format> containers;
    OptionList<int, wav::SampleType> sampleTypes;
    OptionList<int, SamplingRate> samplingRates;
    int sampleTypeId = 1; // sampleTypes.valueId(wav::SAMP_TYPE_INT16);
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
    dsp::stream<dsp::stereo_t> stereoStream;
    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;
    dsp::sink::Handler<float> monoSink;

    dsp::stream<dsp::complex_t> *narrowIQStream = nullptr;
    dsp::filter::DecimatingFIR<dsp::complex_t, float> *decimator = nullptr;
    dsp::tap<float> narrowTaps;
    std::thread runThread;

    dsp::sink::Handler<dsp::complex_t> iqSink;
    dsp::sink::Handler<dsp::stereo_t> dummySink;

    // for RECORDER_MODE_PUREIQ
    dsp::stream<dsp::complex_t> *iqInputStream = nullptr;
    dsp::demod::pureIQ<dsp::complex_t> *iqFilter = nullptr;
    dsp::sink::Handler<dsp::complex_t> pureIQSink;

    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t> *audioStream = NULL;
    // --- Start of Corrected DSP Chain Members ---
    dsp::audio::Volume volume;
    dsp::routing::Splitter<dsp::stereo_t> splitter;
    dsp::stream<dsp::stereo_t> meterStream;
    dsp::bench::PeakLevelMeter<dsp::stereo_t> meter;
    dsp::convert::StereoToMono s2m;
    uint64_t samplerate = 8000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

    std::string currWavFile = "";
    double current = 88000000.0;

    std::atomic<bool> processing = 0;
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

    //=================
    bool flag_akf = false;
    int maxRecShortDur_sec = 3;
    std::string shortRecDirectory = "/var/lib/avr/cws/data/receiver/va_only";
    std::string longRecDirectory = "/var/lib/avr/cws/data/receiver/records";

    std::string host = "localhost"; //"127.0.0.1";
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
    int shutdownPipeFd[2] = {-1, -1}; // [0] = read end, [1] = write end
    int akf_timeout_sec = 0;
    std::atomic<bool> pleaseStopAnalysis{false};
    std::atomic<bool> is_destructing{false};
    int akfUdpPort = 0;

    // Pre-recording buffer variables
    bool preRecord = true;
    int preRecordTimeMs = 500;
    std::unique_ptr<LockFreeRingBuffer> monoPreBuffer;
    size_t preBufferSizeInSamples = 0;
    bool isPreRecord = false;

    bool isPreRecordChannel(const std::string &name)
    {
        if (!preRecord)
            return false;
        if (recMode != RECORDER_MODE_AUDIO)
            return false;
        if (stereo)
            return false;
        if (preRecordTimeMs < 100)
            return false;
        const std::string prefix = "Запис C";
        if (name.rfind(prefix, 0) == 0 && !name.empty())
        {
            char last = name.back();
            if (last >= '1' && last <= '9')
            {
                isPreRecord = true;
                return isPreRecord;
            };
        }
        return false;
    }

    bool meterDetachedForRecord = false;
    bool audioPathRunning = false;
    std::atomic<bool> preBufferDrained{false};
};

std::atomic<bool> RecorderModule::g_stop_workers{false};

MOD_EXPORT void _INIT_()
{
    // При инициализации модуля мы сбрасываем флаг остановки.
    // Потоки должны иметь возможность работать.
    RecorderModule::g_stop_workers.store(false); // <-- ИСПРАВЛЕНО

    // Create default recording directory
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