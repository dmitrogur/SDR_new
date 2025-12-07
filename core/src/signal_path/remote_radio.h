#pragma once

#include <atomic> // <--- ДОБАВЬТЕ ЭТУ СТРОКУ, ЕСЛИ ЕЁ НЕТ
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <dsp/stream.h>
#include <dsp/types.h>
#include "../dsp/compression/sample_stream_compressor.h"
#include "../dsp/sink/handler_sink.h"
#include <utils/optionlist.h>
// #include <utils/networking.h>
#include <gui/smgui.h>
#include <dsp/sink/handler_sink.h> 
#include <json.hpp>

namespace remote
{
    struct InfoHeader
    {
        uint32_t type;
        uint32_t size;
        uint32_t sizeOfExtension;
    };

    struct CommandHeader
    {
        uint32_t cmd;
    };

    enum PacketType
    {
        // Client to Server
        PACKET_TYPE_STATUS,
        PACKET_TYPE_MAIN,
        PACKET_TYPE_MAIN_STAT,
        PACKET_TYPE_COMMAND_ACK,
        PACKET_TYPE_BASEBAND,
        PACKET_TYPE_ERROR,
        PACKET_TYPE_SOURCES,
        PACKET_TYPE_AIRSPY,
        PACKET_TYPE_RADIO,
        PACKET_TYPE_SEARCH,
        PACKET_TYPE_SEARCH_STAT,
        PACKET_TYPE_SCAN,
        PACKET_TYPE_SCAN_STAT,
        PACKET_TYPE_CTRL,
        PACKET_TYPE_RECORD,
        PACKET_TYPE_SINK,
        PACKET_TYPE_SETTINGS
    };

    enum Command
    {
        // Client to Server
        COMMAND_GET_UI = 0x00,
        COMMAND_UI_ACTION,
        COMMAND_START,
        COMMAND_STOP,
        COMMAND_SET_FREQUENCY,
        COMMAND_GET_SAMPLERATE,
        COMMAND_SET_SAMPLE_TYPE,
        COMMAND_SET_COMPRESSION,

        // Server to client
        COMMAND_SET_SAMPLERATE = 0x80,
        COMMAND_DISCONNECT
    };

    enum Error
    {
        ERROR_NONE = 0x00,
        ERROR_INVALID_PACKET,
        ERROR_INVALID_COMMAND,
        ERROR_INVALID_ARGUMENT
    };

    enum RadioInterfaceCommand
    {
        // ...
        RADIO_IFACE_CMD_BIND_REMOTE_STREAM,
        RADIO_IFACE_CMD_UNBIND_REMOTE_STREAM
    };
    // =============================================================
    // НОВОЕ: Потокобезопасная очередь для аудиоданных
    // =============================================================
    template <typename T>
    class ThreadSafeQueue
    {
    public:
        ThreadSafeQueue(size_t max_size = 100) : max_queue_size(max_size) {}

        void push(T value)
        {
            std::lock_guard<std::mutex> lock(mtx);

            // НОВОЕ: Если очередь переполнена, выбрасываем старые данные
            if (queue.size() >= max_queue_size)
            {
                queue.pop_front(); // Удаляем самый старый элемент
            }

            queue.push_back(std::move(value));
            cond.notify_one();
        }

        // Блокируется, пока не появится элемент или не будет вызвана остановка
        bool pop(T &value)
        {
            std::unique_lock<std::mutex> lock(mtx);
            cond.wait(lock, [this]
                      { return !queue.empty() || stop_requested; });

            // Если нас разбудили для остановки и очередь пуста, выходим
            if (stop_requested && queue.empty())
            {
                return false;
            }

            value = std::move(queue.front());
            queue.pop_front();
            return true;
        }

        // Сигнал для остановки ожидания в pop()
        void request_stop()
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop_requested = true;
            cond.notify_all(); // Разбудить все ожидающие потоки
        }
        // Сбрасывает состояние остановки, позволяя использовать очередь заново
        void reset()
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop_requested = false;
        }
    private:
        std::deque<T> queue;
        std::mutex mtx;
        std::condition_variable cond;
        bool stop_requested = false;
        size_t max_queue_size;
    };

    class RemoteRadio
    {
    public:
        RemoteRadio();
        ~RemoteRadio();
        // void init(dsp::stream<dsp::complex_t>* in);
        void init();
        void setInputStream(dsp::stream<dsp::complex_t> *in);
        void start();
        void stop();

        // protected:
        // static void handler(dsp::complex_t* data, int count, void* ctx);

        std::mutex connMtx;
        std::mutex connInfoMtx;

        bool changed = true;
        void infoSinkWorker();
        std::string _streamName;
        std::thread workerThread;
        std::thread senderUDP;
        std::thread senderDbUDP;

        std::atomic<bool> stopworkerThread{false};
        std::atomic<bool> stopUdpSender{false};
        std::atomic<bool> stopUdpDBSender{false};

        // bool _stop = false;
        bool socket_work = false;

        void udpReceiverWorker(const std::string &ip, int port, const std::string &prefix);
        void udpDbWorker(const std::string &ip, int port);
        std::atomic<bool> stopAudioSender{false};
        bool pleaseStop = false;
    private:
        static void ServerHandler(dsp::complex_t* data, int count, void* ctx);
        // static void ServerHandler(uint8_t *data, int count, void *ctx);
        // НОВОЕ: Поток-потребитель для отправки аудио по сети
        void audioSenderWorker();

        // void handleDbsendCommand(uint32_t task_id, int work_status);
        // void processSearchMode(uint32_t task_id);
        // void processScanMode(uint32_t task_id);
        // void processObservationMode(uint32_t task_id);
        void saveConfigToFile(const json &config, const std::string &filename = "sdr_config.json");

        bool _init = false;

        SmGui::DrawListElem dummyElem;
        char hostname[1024];
        int port = 4242;
        // dsp::stream<dsp::complex_t> streamdummyInput;
        // dsp::compression::SampleStreamCompressor comp;
        // НОВОЕ: собственный входной поток для remoteRadio
        dsp::stream<dsp::complex_t> inputStream;
        dsp::sink::Handler<dsp::complex_t> hnd;

        dsp::stream<dsp::complex_t> *curr_stream;
        int numInstance = 0;
        std::string nameInstance = "";
        std::string Version;
        bool isServer;
        bool isNotPlaying;
        bool play_hnd = false;
        int radioMode = 0;

        // --- НОВЫЕ ПОЛЯ ДЛЯ БУФЕРИЗАЦИИ СЕТИ ---
        ThreadSafeQueue<std::vector<dsp::complex_t>> audioQueue{50};
        std::thread audioSenderThread;
        std::atomic<bool> _running{false};
        std::atomic<bool> closeProgramm{false};
        uint16_t info_udp =0;
        
        bool isAsterPlus =false;
        std::atomic<uint32_t> currentTaskId{0};
        std::atomic<int> currentWorkStatus{0}; 
        
        std::string kConnStr;       // Строка подключения к БД
        std::atomic<bool> dbInFlight{false};
        std::atomic<uint32_t> lastDbTaskId{0};
        std::string searchJsonPath, controlJsonPath, scanJsonPath;

        const std::chrono::seconds FORCE_PERIOD = std::chrono::seconds(10);
        char prevTelemetry[512];
        size_t prevCmpLen = 0;
        char prevBuf[512];
        size_t prevBufLen = 0;
        std::chrono::steady_clock::time_point lastSend{
        std::chrono::steady_clock::now() - std::chrono::seconds(10)};        
        bool prevInit = false;
        double prevFrequency = 0.0;
        int prevBandwidth = 0;
        int prevDemodId = -1;
        bool prevIsPlaying = false;
        int prevWorkStatus = -1;
        unsigned prevTaskId = 0;

    };

};