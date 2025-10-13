#include "remote_radio.h"
#include <utils/flog.h>
#include <gui/gui.h>
#include <utils/networking.h>
// #include <server_protocol.h>

#include <arpa/inet.h>  // inet_pton, htons и др.
#include <sys/socket.h> // socket(), sendto()
#include <netinet/in.h> // sockaddr_in
#include <unistd.h>     // close()
#include <cstring>      // strerror

#include "core.h"
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
#include <gui/menus/source.h>
#include <signal_path/sink.h>

namespace remote
{

    std::atomic<bool> stopUdpSender{false};
    std::atomic<bool> stopworkerThread{false};
    struct MainConfig
    {
        uint8_t id;
        int status = 0;
        int id_control = 0;
        int statusServer = 0;
        double freq = 0;
        double offset = 0;
        bool tuningMode = 0;
        int selectedDemodID = 1;
        float bandwidth = 10000;
        bool playing = false;
        float level = 0.7;
        bool recording = false;
        bool search = false;
        int selectedLogicId = 0;
        int idOfList_srch = 0;
        bool scan = false;
        int idOfList_scan = 0;
        bool control = false;
        int idOfList_control = 0;
        int setLevelDbSrch = 0;
        int setLevelDbScan = 0;
        int setLevelDbCtrl = 0;
        bool statAutoLevelSrch = true;
        bool statAutoLevelScan = true;
        bool statAutoLevelCtrl = true;
        int SNRLevelDbSrch = -70;
        bool status_AKFSrch = false;
        bool status_AKFCtrl = false;
        int maxRecWaitTimeCtrl = 5;
        bool UpdateMenu;
        bool isNotPlaying = false;
    };
    // GENERAL STATE
    struct MainStat
    {
        uint8_t id;
        char nameInstance[32];
        char version[32];
        int statusServer = 0;
        double freq;
        double offset;
        bool tuningMode = 0;
        // int selectedDemodID = 1;
        float bandwidth = 10000;
        int sinkOfRadio;
        bool playing = false;
        bool recording = false;
        bool search = false;
        int selectedLogicId = 0;
        int idOfList_srch = 0;
        bool scan = false;
        int idOfList_scan = 0;
        bool control = false;
        int idOfList_control = 0;
        int setLevelDbSrch = 0;
        int setLevelDbScan = 0;
        int setLevelDbCtrl = 0;
        int SNRLevelDbSrch = -70;
        bool status_AKFSrch = false;
        bool status_AKFCtrl = false;
        bool UpdateStat = false;
        double sampleRate = 10000000;
        bool isNotPlaying = false;
    };

    // AIRSPY
    struct AirSpyConfig
    {
        uint8_t id;
        bool Airspy;
        // std::string sources;
        int sourceId;
        int lnaGain;
        int vgaGain;
        int mixerGain;
        int linearGain;
        int sensitiveGain;
        int gainMode;
        bool lnaAgc;
        bool mixerAgc;
        bool select;
        bool _updateLinearGain;
        bool UpdateMenu;
    };
    // RADIO
    struct RadioConfig
    {
        int selectedDemodID = 1;
        int snapInterval = 0;
        int snapIntervalId = 0;
        int deempId = 0;
        int baseband_band = 1000000;
        int fmIFPresetId = 0;
        int tone = 800;

        float bandwidth = 10000;
        float nbLevel = 10.0f;
        float squelchLevel = 0;
        float agcAttack = 50.0f;
        float agcDecay = 5.0f;

        bool nbEnabled = false;
        bool squelchEnabled = false;
        bool FMIFNREnabled = false;
        bool _lowPass = true;
        bool _highPass = false;
        bool carrierAgc = false;
        bool UpdateMenu = false;
    };
    // RECORD
    struct RecordConfig
    {
        bool recording = 1;
        bool UpdateMenu = false;
    };
    // SEARCH
    struct SearchConfig
    {
        int selectedLogicId = 0;
        int idOfList_srch = 0;
        bool button_srch = true;
        int levelDb = -50;
        int SNRlevelDb = 10;
        bool status_AKF = false;
        bool UpdateModule = false;
        bool UpdateLists = false;
        bool UpdateMenu = false;
        bool statAutoLevel = false;
    };
    // SEARCH STAT
    struct FoundBookmark
    {
        double frequency;
        float bandwidth;
        int mode;
        int level;
        bool selected;
        std::time_t ftime;
        int Signal = -1;
    };
    // std::string sources;
    // SCAN
    struct ScanConfig
    {
        int idOfList_scan = 0;
        bool button_scan = false;
        int maxRecDuration = 5;
        int maxRecWaitTime = 10;
        bool flag_level = false;
        int level = -50;
        bool UpdateModule = false;
        bool UpdateLists = false;
        bool UpdateMenu = false;
        bool statAutoLevel = false;
    }; // CTRL
    struct CTRLConfig
    {
        int idOfList_ctrl = 0;
        bool button_ctrl = false;
        bool button_scan = false;
        int maxRecWaitTime = 10;
        bool flag_level = false;
        int level = -50;
        bool UpdateModule = false;
        bool UpdateLists = false;
        bool UpdateMenu = false;
        bool statAutoLevel = false;
        bool status_AKF = false;
    };
    net::Conn client;
    net::Listener listener;
    net::Listener listenerInfo;
    net::Conn conn;
    net::Conn connInfo;
    uint8_t *bbuf = NULL;
    // std::unique_ptr<uint8_t[]> bbuf;
    InfoHeader *bb_pkt_hdr = NULL;
    uint8_t *bb_pkt_data = NULL;
    // uint8_t* ib_pkt_data = NULL;

    // В файле remote_radio.cpp

    // В файле remote_radio.cpp

    static void clientHandler(net::Conn client_conn, void *ctx)
    {
        RemoteRadio *_this = (RemoteRadio *)ctx;

        // Этот обработчик вызывается библиотекой net в НОВОМ потоке для КАЖДОГО нового клиента.

        // flog::error("!!!!!!!!!! [SERVER] clientHandler ENTERED for a new client !!!!!!!!!!");

        // 1. Устанавливаем нового клиента как текущего
        {
            std::lock_guard lck(_this->connMtx);
            // Если уже было активное соединение, вежливо закрываем его.
            if (conn && conn->isOpen())
            {
                flog::warn("[clientHandler] Another client connected, closing the old one.");
                conn->close();
            }
            conn = std::move(client_conn);
        }

        // 2. Сразу же снова начинаем слушать следующего клиента.
        //    Это позволит принять новое подключение, даже пока мы работаем с текущим.
        if (listener && !_this->stopAudioSender)
        { // Проверяем, что listener еще жив
            listener->acceptAsync(clientHandler, _this);
            flog::info("[clientHandler] Re-armed listener to accept next client.");
        }

        // 3. Ждем, пока этот клиент не отвалится.
        //    Поток audioSenderWorker в это время будет с ним работать.
        /// flog::info(">>>> [clientHandler] Now waiting for current client to disconnect...");
        conn->waitForEnd();
        // flog::info("<<<< [clientHandler] Client disconnected.");

        // 4. Очищаем соединение.
        {
            std::lock_guard lck(_this->connMtx);
            // Убедимся, что мы удаляем именно тот сокет, с которым работали, а не новый, если кто-то уже подключился.
            if (conn == client_conn)
            {
                conn = nullptr;
            }
        }
        // flog::info("!!!!!!!!!! [SERVER] clientHandler finished for one client. !!!!!!!!!!");
    }

    static void clientInfoHandler(net::Conn client2, void *ctx)
    {
        RemoteRadio *_this = (RemoteRadio *)ctx;
        std::lock_guard lck(_this->connInfoMtx);

        {
            connInfo = std::move(client2);
        }

        if (connInfo)
        {
            _this->changed = true;
            flog::info("rcv connInfo changed {0}", _this->changed);

            stopworkerThread = true;
            _this->socket_work = true;
            if (_this->workerThread.joinable())
            {
                _this->workerThread.join();
            }

            stopworkerThread = false;

            flog::info("workerThread!!");

            _this->workerThread = std::thread(&RemoteRadio::infoSinkWorker, _this);
            gui::mainWindow.setstatusControl(0, ARM_STATUS_STAT_CONTROL);

            connInfo->waitForEnd();
            connInfo->close();
        }
        else
        {
        }
        listenerInfo->acceptAsync(clientInfoHandler, _this);
    }

    // =============== SERVER ====================================
    RemoteRadio::RemoteRadio()
    {
        // comp.init(&inputStream, dsp::compression::PCM_TYPE_I8); // или I16, Q16
        // hnd.init(&comp.out, ServerHandler, this);
        hnd.init(&inputStream, ServerHandler, this);
        _init = true;
        flog::info("  RemoteRadio::RemoteRadio!!!");
        isServer = false;
        closeProgramm.store(false);
    }

    RemoteRadio::~RemoteRadio()
    {
        flog::info("RemoteRadio destructor called.");
        closeProgramm.store(true);
        stop();
        if (bbuf)
        {
            delete[] bbuf;
            bbuf = nullptr;
        }
    }

    // void RemoteRadio::init(dsp::stream<dsp::complex_t> *stream)
    void RemoteRadio::init()
    {
        flog::info("  RemoteRadio::init!!!");
        int InstNum = 0;
        bool _update = false;
        port = 4200;
        core::configManager.acquire();
        try
        {
            isServer = core::configManager.conf["IsServer"];
            InstNum = core::configManager.conf["InstanceNum"];
            numInstance = core::configManager.conf["InstanceNum"];
            nameInstance = core::configManager.conf["InstanceName"];
        }
        catch (const std::exception &e)
        {
            isServer = false;
            std::cerr << e.what() << '\n';
        }
        Version = std::string(VERSION_STR);

        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            radioMode = 0;
        }
        bool available = core::configManager.conf.contains("IsNotPlaying");
        if (available)
        {
            isNotPlaying = core::configManager.conf["IsNotPlaying"];
        }
        else
        {
            isNotPlaying = false;
            core::configManager.conf["IsNotPlaying"] = isNotPlaying;
            _update = true;
        }
        if (isServer)
        {
            try
            {
                port = core::configManager.conf["ServerPort"];
            }
            catch (const std::exception &e)
            {
                port = 4200 + InstNum * 10;
                core::configManager.conf["ServerPort"] = port;
                _update = true;
                std::cerr << e.what() << '\n';
            }
        }
        if (_update)
            core::configManager.release(true);
        else
            core::configManager.release();

        // isServer = true;
        if (!isServer)
            return;

        /// inBuf.init(stream);
        // inBuf.bypass = !buffering;
        // comp.init(stream, dsp::compression::PCM_TYPE_I8);
        // hnd.init(&comp.out, ServerHandler, this); // _testServerHandler, this);
        bbuf = new uint8_t[MAX_PACKET_SIZE];
        bb_pkt_hdr = (InfoHeader *)bbuf;
        bb_pkt_data = &bbuf[sizeof(InfoHeader)];

        // _init = true;
        stopworkerThread = false;
        stopUdpSender = false;
    }

    // startListener
    void RemoteRadio::start()
    {
        if (!isServer)
            return;

        std::string host = "0.0.0.0";
        flog::info("startServer ! {0}:{1}", host, port);
        gui::mainWindow.setFirstConn(ALL_MOD, true);

        listenerInfo = net::listen(host, port + 2);

        if (listenerInfo)
        {
            listenerInfo->acceptAsync(clientInfoHandler, this); // this
            flog::info("Ready, listening on {0}:{1}", host, port + 2);
            // while(1) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }

        flog::info("RemoteRadio::start() called.");
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            flog::warn("RemoteRadio::start() called, but it is already running.");
            return;
        }

        flog::info("RemoteRadio::start() - First entry. Starting components...");

        sigpath::iqFrontEnd.tempStop();
        sigpath::iqFrontEnd.bindIQStream(&inputStream);
        sigpath::iqFrontEnd.tempStart();

        hnd.start();

        stopAudioSender = false;

        if (radioMode == 0)
            senderUDP = std::thread(&RemoteRadio::udpSenderThread, this, "192.168.88.11", 45712, "rpm1v.1.2.4");
        else
            senderUDP = std::thread(&RemoteRadio::udpSenderThread, this, "192.168.30.100", 45712, "rpm1v.1.2.4"); // 172.128.150.1
        if (!isNotPlaying)
        {
            // НОВОЕ: Перед запуском comp и hnd, подписываем наш внутренний поток на IQFrontEnd
            if (!audioSenderThread.joinable())
            {
                audioSenderThread = std::thread(&RemoteRadio::audioSenderWorker, this);
            }
            // Запуск TCP-слушателя
            try
            {
                std::string host = "0.0.0.0";
                listener = net::listen(host, port);
                if (listener)
                {
                    listener->acceptAsync(clientHandler, this);
                    // clientHandlerThread = std::thread(clientHandler, std::move(net::Conn()), this);
                    flog::info("Ready, listening on {0}:{1}", host, port);
                    flog::info("Ready, listening on {0}:{1}", host, port);
                }
            }
            catch (const std::exception &e)
            {
                flog::error("Could not start TCP listener: {0}", e.what());
            }
        }
    }

    // В remote_radio.cpp

    void RemoteRadio::stop()
    {
        bool expected = true;
        if (!_running.compare_exchange_strong(expected, false))
        {
            return;
        }
        flog::info("RemoteRadio::stop() - Shutting down...");

        // Устанавливаем флаги, чтобы потоки знали, что нужно завершаться
        stopAudioSender = true;
        stopUdpSender = true;
        stopworkerThread = true;

        // Закрываем сокеты, чтобы разблокировать потоки
        if (conn)
        {
            conn->close();
        }
        if (connInfo)
        {
            connInfo->close();
        } // <-- ДОБАВЛЕНО
        if (listener)
        {
            listener->close();
        }
        if (listenerInfo)
        {
            listenerInfo->close();
        }

        if (workerThread.joinable())
        {
            workerThread.join(); // <--- ВИСНЕТ ЗДЕСЬ
        }
        if (audioSenderThread.joinable())
        {
            audioQueue.request_stop();
            audioSenderThread.join();
        }
        if (workerThread.joinable())
        { // <-- ДОБАВЛЕНО
            workerThread.join();
        }
        if (senderUDP.joinable())
        {
            senderUDP.join();
        }

        hnd.stop();

        if (_init && sigpath::iqFrontEnd.isInitialized())
        {
            sigpath::iqFrontEnd.unbindIQStream(&inputStream);
        }
        flog::info("RemoteRadio::stop() finished successfully.");
    }
    /*
    void RemoteRadio::stop()
    {
        bool expected = true;
        if (!_running.compare_exchange_strong(expected, false))
        {
            return;
        }
        flog::info("RemoteRadio::stop() - Shutting down...");

        // 2. Закрываем активное DATA соединение.
        if (conn)
        { // <-- Здесь isOpen() нужен и он есть у net::Conn
            conn->close();
        }
        flog::info("RemoteRadio::stop() - conn->close() OK");
        // if (closeProgramm.load())
        // {
        if (connInfo)
        {
            connInfo->close();
        }
        flog::info("RemoteRadio::stop() - conn->close() OK");
        //}

        // 1. Закрываем слушающий сокет.
        if (listener)
        { // <-- УБРАЛИ ПРОВЕРКУ isOpen()
            listener->close();
        }

        flog::info("RemoteRadio::stop() - listener->close OK");

        if (listenerInfo)
        {
            listenerInfo->close();
        }
        flog::info("RemoteRadio::stop() - listenerInfo->close OK");

        if (closeProgramm.load())
        {
            // 3. Останавливаем все потоки
            if (audioSenderThread.joinable())
            {
                stopAudioSender = true;
                audioQueue.request_stop();
                audioSenderThread.join();
            }
            flog::info("RemoteRadio::connInfo() - audioSenderThread OK");
        }

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        flog::info("RemoteRadio::connInfo() - workerThread OK");

        if (senderUDP.joinable())
        {
            stopUdpSender = true;
            senderUDP.join();
        }

        flog::info("RemoteRadio::connInfo() - senderUDP OK");

        // 4. Останавливаем DSP
        hnd.stop();

        flog::info("RemoteRadio::connInfo() - hnd.stop OK");

        // 5. Отписываемся от iqFrontEnd
        if (_init && sigpath::iqFrontEnd.isInitialized())
        {
            sigpath::iqFrontEnd.unbindIQStream(&inputStream);
        }

        flog::info("RemoteRadio::stop() finished successfully.");
    }
    */
    void RemoteRadio::audioSenderWorker()
    {
        flog::info("IQ sender thread started.");

        while (!stopAudioSender)
        {
            // --- ФАЗА 1: ЖДАТЬ КЛИЕНТА ---
            // Используем `while` вместо `if`. Поток будет здесь "висеть",
            // пока `conn` не станет валидным или пока программу не закроют.
            while ((!conn || !conn->isOpen()) && !stopAudioSender)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Если вышли из цикла ожидания, потому что программу закрывают
            if (stopAudioSender)
            {
                break;
            }

            flog::info("IQ SENDER: Client is connected. Starting data transmission.");

            // --- ФАЗА 2: ПЕРЕДАЧА ДАННЫХ ---
            std::vector<dsp::complex_t> data_chunk;
            // Цикл, пока соединение живо и программу не закрывают
            while (conn && conn->isOpen() && !stopAudioSender)
            {
                // Используем твой метод pop(), который блокирует, пока не появятся данные
                // или пока очередь не будет остановлена.
                if (audioQueue.pop(data_chunk))
                {
                    // Данные получены из очереди
                    size_t data_size_bytes = data_chunk.size() * sizeof(dsp::complex_t);
                    if (conn->write(data_size_bytes, reinterpret_cast<uint8_t *>(data_chunk.data())) <= 0)
                    {
                        // Ошибка записи = клиент отключился.
                        flog::warn("IQ SENDER: Write failed, client disconnected.");
                        // Закрываем сокет, чтобы и другие части программы знали об обрыве
                        conn->close();
                        break; // Выходим из внутреннего цикла отправки
                    }
                }
                else
                {
                    // audioQueue.pop() вернула false, значит, была вызвана request_stop().
                    // Это сигнал к завершению потока.
                    break; // Выходим из внутреннего цикла отправки
                }
            }
            flog::info("IQ SENDER: Data loop finished. Waiting for new client.");
        }
        flog::info("IQ sender thread stopped.");
    }

    void RemoteRadio::setInputStream(dsp::stream<dsp::complex_t> *stream)
    {
        // Этот метод теперь, по сути, не нужен, так как мы подписываемся
        // на iqFrontEnd напрямую. Но если он где-то вызывается, он должен
        // либо ничего не делать, либо переназначать вход у `hnd`.
        // Давайте пока оставим его пустым, чтобы не было ошибок.
        // Если `comp` закомментирован, то эти строки вызовут ошибку:
        // comp.setPCMType(dsp::compression::PCM_TYPE_I16);
        // comp.setInput(curr_stream);
        flog::info("setInputStream - doing nothing now.");
    }

    void RemoteRadio::infoSinkWorker()
    {
        flog::info("  infoSinkWorker!!!");
        /*
        Синхронный блокирующий цикл:
            Где: infoSinkWorker и workerInfo.
        Проблема: Как описано выше, цикл управления строго синхронный. Если клиент "зависнет" и не отправит ответ, поток infoSinkWorker на сервере будет заблокирован навсегда на вызове connInfo->read(). Это делает сервер уязвимым к нестабильным клиентам или проблемам в сети.
        Решение (сложное, на будущее): Переход на асинхронную модель. Вместо цикла "отправить-прочитать" сделать два независимых действия:
        Один механизм для отправки обновлений по мере их возникновения.
        Другой механизм (в том же или отдельном потоке) для постоянного прослушивания входящих команд. Это усложняет код, но делает его гораздо более отказоустойчивым.
        */
        // if(!isServer) return;
        uint8_t *ibuf = NULL;
        InfoHeader *ib_pkt_hdr = NULL;

        ibuf = new uint8_t[256 * MAX_STRUCT_SIZE];
        ib_pkt_hdr = (InfoHeader *)ibuf;

        // StatusServerConfig msgStatusServer;
        MainConfig msgMain;
        MainStat msgMainStat;
        AirSpyConfig msgAir;
        RadioConfig msgRadio;
        RecordConfig msgRecord;
        SearchConfig msgSearch;
        ScanConfig msgScan;
        CTRLConfig msgCTRL;

        FoundBookmark msgStatFinded;

        // gui::mainWindow.setUpdateMenuSnd(true);
        gui::mainWindow.setUpdateMenuSnd0Main(0, true);
        gui::mainWindow.setUpdateMenuSnd2Radio(true);
        // gui::mainWindow.setUpdateMenuSnd3Record(true);
        gui::mainWindow.setUpdateLists_srch(false);
        gui::mainWindow.setUpdateMenuSnd5Srch(0, false);
        gui::mainWindow.setUpdateLists_scan(false);
        gui::mainWindow.setUpdateMenuSnd6Scan(0, false);
        gui::mainWindow.setUpdateMenuSnd7Ctrl(0, false);

        socket_work = false;
        int ms_interval = 250;
        int change_freq = 0;
        while (!stopworkerThread)
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(ms_interval));
            {
                if (stopworkerThread)
                    break;
                int SrvStatus = gui::mainWindow.getstatusControl(0);
                // flog::info("1");
                if (connInfo && connInfo->isOpen())
                {
                    // if (changed == true)
                    double _freq = gui::waterfall.getCenterFrequency();
                    double _offset = 0;
                    int _tuningMode = gui::mainWindow.gettuningMode();
                    if (_tuningMode != tuner::TUNER_MODE_CENTER)
                    {
                        std::string nameVFO = "Канал приймання"; // _streamName; // gui::waterfall.selectedVFO;
                        if (gui::waterfall.vfos.find(nameVFO) != gui::waterfall.vfos.end())
                        {
                            _offset = gui::waterfall.vfos[nameVFO]->generalOffset;
                        }
                    }
                    bool send_airspy = false;
                    bool send_radio = false;
                    bool send_main = false;
                    bool send_record = false;

                    // flog::info("TRACE SND SrvStatus {0}, ", SrvStatus);
                    if (SrvStatus == ARM_STATUS_FULL_CONTROL && gui::mainWindow.getUpdateMenuSnd())
                    {
                        msgAir.Airspy = true;
                        msgAir.sourceId = sourcemenu::getSourceId();
                        msgAir.lnaGain = gui::mainWindow.getlnaGain();
                        msgAir.vgaGain = gui::mainWindow.getvgaGain();
                        msgAir.mixerGain = gui::mainWindow.getmixerGain();
                        msgAir.linearGain = gui::mainWindow.getlinearGain();
                        msgAir.sensitiveGain = gui::mainWindow.getsensitiveGain();
                        msgAir.gainMode = gui::mainWindow.getgainMode();
                        msgAir.lnaAgc = gui::mainWindow.getlnaAgc();
                        msgAir.mixerAgc = gui::mainWindow.getmixerAgc();
                        msgAir.select = gui::mainWindow.getselect();
                        msgAir._updateLinearGain = gui::mainWindow.get_updateLinearGain();
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgAir);
                        ib_pkt_hdr->type = PACKET_TYPE_AIRSPY;
                        ib_pkt_hdr->sizeOfExtension = 0;
                        if (SrvStatus == 2)
                            msgAir.UpdateMenu = true;
                        else
                            msgAir.UpdateMenu = false;
                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgAir, sizeof(msgAir));
                        flog::info("TRACE PACKET_TYPE_AIRSPY SND  msgAir.gainMode {0}, msgAir.linearGain{1}, lnaGain {2}", msgAir.gainMode, msgAir.linearGain, msgAir.lnaGain);
                        // gui::mainWindow.setUpdateMenuSnd(false);
                        send_airspy = true;
                    }
                    else if (SrvStatus == ARM_STATUS_FULL_CONTROL && gui::mainWindow.getUpdateMenuSnd2Radio())
                    { // RADIO
                        msgRadio.selectedDemodID = gui::mainWindow.getselectedDemodID();
                        msgRadio.bandwidth = gui::mainWindow.getbandwidth();
                        msgRadio.snapInterval = gui::mainWindow.getsnapInterval();
                        msgRadio.snapIntervalId = gui::mainWindow.getsnapIntervalId();
                        msgRadio.deempId = gui::mainWindow.getdeempId();
                        msgRadio.nbEnabled = gui::mainWindow.getnbEnabled();
                        msgRadio.nbLevel = gui::mainWindow.getnbLevel();
                        msgRadio.baseband_band = gui::mainWindow.getCMO_BBand();
                        msgRadio.squelchEnabled = gui::mainWindow.getsquelchEnabled();
                        msgRadio.squelchLevel = gui::mainWindow.getsquelchLevel();
                        msgRadio.FMIFNREnabled = gui::mainWindow.getFMIFNREnabled();
                        msgRadio.fmIFPresetId = gui::mainWindow.getfmIFPresetId();
                        msgRadio._lowPass = gui::mainWindow.getlowPass();
                        msgRadio._highPass = gui::mainWindow.gethighPass();
                        msgRadio.agcAttack = gui::mainWindow.getagcAttack();
                        msgRadio.agcDecay = gui::mainWindow.getagcDecay();
                        msgRadio.carrierAgc = gui::mainWindow.getcarrierAgc();
                        msgRadio.UpdateMenu = true;
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgRadio);
                        ib_pkt_hdr->type = PACKET_TYPE_RADIO;
                        ib_pkt_hdr->sizeOfExtension = 0;
                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgRadio, sizeof(msgRadio));
                        flog::info("TRACE SND PACKET_TYPE_RADIO type {0},  msgRadio.selectedDemodID {1}", ib_pkt_hdr->type, msgRadio.selectedDemodID);
                        // gui::mainWindow.setUpdateMenuSnd2Radio(false);
                        send_radio = true;
                    }
                    else if (gui::mainWindow.getUpdateMenuSnd3Record()) // SrvStatus == ARM_STATUS_FULL_CONTROL &&
                    {                                                   // RADIO
                        msgRecord.recording = gui::mainWindow.getServerRecording(0); // gui::mainWindow.getRecording();
                        if (SrvStatus == 2)
                            msgRecord.UpdateMenu = true;
                        else
                            msgRecord.UpdateMenu = false;
                        flog::info("PACKET_TYPE_RECORD. Send.  gui::mainWindow.getServerRecording(0) = {0}", gui::mainWindow.getServerRecording(0));
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgRecord);
                        ib_pkt_hdr->sizeOfExtension = 0;
                        ib_pkt_hdr->type = PACKET_TYPE_RECORD;
                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgRecord, sizeof(msgRecord));
                        gui::mainWindow.setUpdateMenuSnd3Record(false);
                        send_record = true;
                    }
                    else if (SrvStatus == ARM_STATUS_FULL_CONTROL && gui::mainWindow.getUpdateMenuSnd0Main(0) == true)
                    {
                        msgMain.id = numInstance;
                        msgMain.freq = _freq; // htons(_freq);
                        msgMain.offset = _offset;
                        msgMain.status = gui::mainWindow.getstatusControl(0);
                        msgMain.tuningMode = _tuningMode;
                        msgMain.playing = gui::mainWindow.isPlaying();
                        msgMain.isNotPlaying = isNotPlaying;

                        /*
                        SinkManager::Stream *_stream = sigpath::sinkManager.getCurrStream("Канал приймання");
                        if (_stream != NULL)
                            msgMain.level = _stream->getVolume();
                        else
                            msgMain.level = 0.5;
                        */
                        // flog::info("\n PACKET_TYPE_MAIN. SEND. msgMain.tuningMode {0}, _tuningMode {1}, msgMain.playing {2}, msgMain.status {3}, msgMain.level {4}, _freq {5}, _offset {6}\n", msgMain.tuningMode, _tuningMode, msgMain.playing, msgMain.status, msgMain.level, _freq, _offset);
                        if (SrvStatus == ARM_STATUS_FULL_CONTROL)
                            msgMain.UpdateMenu = true;
                        else
                            msgMain.UpdateMenu = false;
                        ib_pkt_hdr->type = PACKET_TYPE_MAIN;
                        ib_pkt_hdr->sizeOfExtension = 0;
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgMain);
                        // flog::info("TRACE SND 10 Else type {0}", ib_pkt_hdr->type);
                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgMain, sizeof(msgMain));
                        send_main = true;
                        gui::mainWindow.setUpdateMenuSnd0Main(0, false);
                    }
                    else if (SrvStatus == ARM_STATUS_FULL_CONTROL && gui::mainWindow.getUpdateStatSnd5Srch() == true)
                    { // SEARCH STAT
                        int sizeofbbuf = sizeof(InfoHeader);
                        std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                        for (const auto &[key, findbm] : gui::waterfall.finded_freq)
                        {
                            msgStatFinded.frequency = findbm.frequency;
                            msgStatFinded.bandwidth = findbm.bandwidth;
                            msgStatFinded.mode = findbm.mode;
                            msgStatFinded.level = findbm.level;
                            msgStatFinded.selected = findbm.selected;
                            msgStatFinded.ftime = findbm.ftime;
                            msgStatFinded.Signal = findbm.Signal;
                            // std::ostringstream ss;
                            // ss << std::put_time(std::localtime(&findbm.ftime), "%y%m%d%H%M%S");
                            // flog::info("TRACE! sgStatFinded.frequency {0}, sizeof(msgStatFinded) {1}, findbm.ftime {2}", msgStatFinded.frequency, sizeof(msgStatFinded), ss.str());
                            memcpy(&ibuf[sizeofbbuf], (void *)&msgStatFinded, sizeof(msgStatFinded));
                            sizeofbbuf = sizeofbbuf + sizeof(msgStatFinded);
                        }
                        //}
                        ib_pkt_hdr->type = PACKET_TYPE_SEARCH_STAT;
                        ib_pkt_hdr->size = sizeofbbuf;
                        ib_pkt_hdr->sizeOfExtension = 0;
                        // memcpy(&ibuf[ib_pkt_hdr->size], (void*)gui::mainWindow.getbbuf_stat_srch(), ib_pkt_hdr->sizeOfExtension);
                        // msgStatFinded statFreq;
                        /*
                        StatFoundBookmark statFreq;
                        uint32_t count = sizeofbbuf - sizeof(InfoHeader);
                        flog::info("PACKET_TYPE_SEARCH_STAT. Send.  msgStatFinded.frequency {0}, msgStatFinded.bandwidth {1}, msgStatFinded.mode {2}, ib_pkt_hdr->size {3}, count {4}, sizeof(statFreq) {5}", msgStatFinded.frequency, msgStatFinded.bandwidth, msgStatFinded.mode, ib_pkt_hdr->size, count, sizeof(StatFoundBookmark));

                        for (int poz = sizeof(InfoHeader); poz < sizeofbbuf; poz = poz + sizeof(StatFoundBookmark)) {
                            memcpy((void*)&statFreq, (void*) (bbuf + poz), sizeof(StatFoundBookmark));
                            //memcpy((void*)&statFreq, (void*) (bbufRCV + poz), sizeof(StatFoundBookmark));
                            flog::info("TRACE! gui::waterfall.addFreq.frequency = {0}, msgStatFinded.bandwidth {1},  poz {2}", statFreq.frequency, statFreq.bandwidth, poz);
                        }
                        */
                        gui::mainWindow.setUpdateStatSnd5Srch(false);
                    }
                    else
                    { // SEND STAT
                        if (SrvStatus > ARM_STATUS_NOT_CONTROL)
                        {
                            msgMainStat.id = numInstance;
                            strcpy(msgMainStat.nameInstance, nameInstance.c_str());
                            strcpy(msgMainStat.version, Version.c_str());
                            msgMainStat.statusServer = SrvStatus;

                            // _freq = _freq  + gui::waterfall.getViewOffset();
                            /*

                            if (SrvStatus == ARM_STATUS_FULL_CONTROL) { // && change_freq > 0) {
                                msgMainStat.freq = 0;
                                msgMainStat.offset = 0;
                            }
                            else {
                            */
                            // double _freq = gui::waterfall.getCenterFrequency();
                            msgMainStat.freq = _freq;

                            int _tuningMode = gui::mainWindow.gettuningMode();
                            if (_tuningMode != tuner::TUNER_MODE_CENTER)
                            {
                                std::string nameVFO = "Канал приймання"; // _streamName; // gui::waterfall.selectedVFO;
                                if (gui::waterfall.vfos.find(nameVFO) != gui::waterfall.vfos.end())
                                {
                                    _offset = gui::waterfall.vfos[nameVFO]->generalOffset;
                                }
                            }
                            else
                                _offset = 0;
                            msgMainStat.offset = _offset;
                            // }
                            msgMainStat.tuningMode = _tuningMode;
                            // msgMainStat.selectedDemodID = gui::mainWindow.getselectedDemodID();
                            msgMainStat.bandwidth = gui::mainWindow.getbandwidth();
                            msgMainStat.sinkOfRadio = 1;
                            msgMainStat.playing = gui::mainWindow.isPlaying(); // false;
                            msgMainStat.isNotPlaying = isNotPlaying;
                            msgMainStat.recording = gui::mainWindow.getServerRecording(0); // gui::mainWindow.getRecording();
                            msgMainStat.search = gui::mainWindow.getbutton_srch(0);
                            msgMainStat.selectedLogicId = gui::mainWindow.getselectedLogicId(0);
                            msgMainStat.idOfList_srch = gui::mainWindow.getidOfList_srch(0);
                            msgMainStat.scan = gui::mainWindow.getbutton_scan(0);
                            msgMainStat.idOfList_scan = gui::mainWindow.getidOfList_scan(0);
                            msgMainStat.control = gui::mainWindow.getbutton_ctrl(0);
                            msgMainStat.idOfList_control = gui::mainWindow.getidOfList_ctrl(0);
                            msgMainStat.setLevelDbSrch = gui::mainWindow.getLevelDbSrch(0);
                            msgMainStat.setLevelDbScan = gui::mainWindow.getLevelDbScan(0);
                            msgMainStat.setLevelDbCtrl = gui::mainWindow.getLevelDbCtrl(0);
                            // msgMainStat.statAutoLevelSrch = gui::mainWindow.getAuto_levelSrch(0);
                            msgMainStat.SNRLevelDbSrch = gui::mainWindow.getSNRLevelDb(0);
                            msgMainStat.status_AKFSrch = gui::mainWindow.getAKFInd(0);
                            msgMainStat.status_AKFCtrl = gui::mainWindow.getAKFInd_ctrl(0);
                            msgMainStat.sampleRate = sigpath::iqFrontEnd.getSampleRate();

                            // msgMainStat.statAutoLevelCtrl = gui::mainWindow.getAuto_levelCtrl(0);
                            ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgMainStat);
                        }
                        else
                        {
                            ib_pkt_hdr->size = sizeof(InfoHeader);
                        }
                        if (SrvStatus == ARM_STATUS_FULL_CONTROL)
                        {
                            ms_interval = 200; /// 500...1000
                        }
                        ib_pkt_hdr->type = PACKET_TYPE_MAIN_STAT;
                        ib_pkt_hdr->sizeOfExtension = 0;
                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgMainStat, sizeof(msgMainStat));
                        /// flog::info("TRACE SND MAIN STAT type {0}, freq {1},  UpdateStat {2}, selectedDemodID {3}, selectedLogicId {4}, SrvStatus {5}, msgMainStat.freq {6}, Offset {7}, msgMainStat.statusServer {8}", ib_pkt_hdr->type, msgMainStat.freq, msgMainStat.UpdateStat, gui::mainWindow.getselectedDemodID(), msgMainStat.selectedLogicId, SrvStatus, msgMainStat.freq, gui::waterfall.getViewOffset(), msgMainStat.statusServer);
                    }

                    // Write to network
                    //=================== ->write
                    if (connInfo && connInfo->isOpen())
                    {
                        connInfo->write(ib_pkt_hdr->size, ibuf);
                    }
                    // flog::info("->writeInfo ib_pkt_hdr->type  {0}", ib_pkt_hdr->type);
                    //============================================================================================================================

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    //============================================================================================================================
                    //=================== read <-
                    int read = 0;
                    int count = sizeof(InfoHeader); //  + sizeof(msgMain)
                    int len = 0;
                    read = 0;
                    ib_pkt_hdr->size = 0;
                    int i = 0;
                    while (len < count)
                    {
                        read = connInfo->read(count - len, &ibuf[len]);
                        // flog::info("connInfo->read {0}, ib_pkt_hdr->size {1},  ib_pkt_hdr->type {2}, ib_pkt_hdr->sizeOfExtension {3}, count - len {4}", read, ib_pkt_hdr->size, ib_pkt_hdr->type, ib_pkt_hdr->sizeOfExtension, (count - len));

                        if (read != (count - len))
                        {
                            stopworkerThread = true;
                            connInfo->waitForEnd();
                        };
                        if (stopworkerThread == true)
                            break;

                        if (i == 0)
                        {
                            // ==== НАЧАЛО ИЗМЕНЕНИЯ ====
                            if (ib_pkt_hdr->size > (256 * MAX_STRUCT_SIZE)) //  || ib_pkt_hdr->size < sizeof(InfoHeader))
                            {
                                flog::error("Received packet with invalid size: {0}. Ignoring and flushing socket.", ib_pkt_hdr->size);
                                // Выходим из цикла чтения этого пакета
                                len = count; // Искусственно завершаем цикл while (len < count)
                                continue;    // Переходим к следующей итерации
                            } // ==== КОНЕЦ ИЗМЕНЕНИЯ ====
                            count = ib_pkt_hdr->size; // - sizeof(InfoHeader);;
                        }
                        i++;
                        len += read;
                    }
                    if (stopworkerThread == true)
                        break;
                    // flog::info("connInfo->read 2 OK.  count {0}, ib_pkt_hdr->type {1}, SrvStatus {2}!", count, ib_pkt_hdr->type, SrvStatus);
                    count = ib_pkt_hdr->size - sizeof(InfoHeader);
                    if (ib_pkt_hdr->type == PACKET_TYPE_AIRSPY && SrvStatus == ARM_STATUS_FULL_CONTROL)
                    {
                        memcpy(&msgAir, &ibuf[sizeof(InfoHeader)], count);
                        // flog::info("connInfo->read 2 count {0}, msgAir.lnaGain {1},  msgAir.UpdateMenu {2}, ib_pkt_hdr->type {3}, vgaGain {4}", count, msgAir.lnaGain, msgAir.UpdateMenu, ib_pkt_hdr->type, msgAir.vgaGain);
                        if (msgAir.UpdateMenu)
                        {
                            flog::info("TRACE RCV (msgAir.UpdateMenu) {0}, msgAir.linearGain {1}, lnaGain {2}, msgAir.lnaGain {3}, vgaGain {4}", msgAir.UpdateMenu, msgAir.linearGain, msgAir.lnaGain, msgAir.lnaGain, msgAir.vgaGain);
                            gui::mainWindow.setlnaGain(msgAir.lnaGain);
                            gui::mainWindow.setvgaGain(msgAir.vgaGain);
                            gui::mainWindow.setmixerGain(msgAir.mixerGain);
                            gui::mainWindow.setlinearGain(msgAir.linearGain);
                            gui::mainWindow.setsensitiveGain(msgAir.sensitiveGain);
                            gui::mainWindow.setgainMode(msgAir.gainMode);
                            gui::mainWindow.setlnaAgc(msgAir.lnaAgc);
                            gui::mainWindow.setmixerAgc(msgAir.mixerAgc);
                            gui::mainWindow.setselect(msgAir.select);
                            gui::mainWindow.set_updateLinearGain(msgAir._updateLinearGain);
                            gui::mainWindow.setUpdateMenuRcv(true);
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_RADIO && SrvStatus == ARM_STATUS_FULL_CONTROL)
                    { // RADIO
                        memcpy(&msgRadio, &ibuf[sizeof(InfoHeader)], count);
                        if (msgRadio.UpdateMenu)
                        {
                            flog::info("TRACE PACKET_TYPE_RADIO RCV (msgRadio.UpdateMenu) {0}, selectedDemodID {1}, SrvStatus {2}, msgRadio.snapInterval {3}, msgRadio.bandwidth {4}", msgRadio.UpdateMenu, msgRadio.selectedDemodID, SrvStatus, msgRadio.snapInterval, msgRadio.bandwidth);
                            gui::mainWindow.setselectedDemodID(msgRadio.selectedDemodID);
                            gui::mainWindow.setbandwidth(msgRadio.bandwidth);
                            gui::mainWindow.setsnapInterval(msgRadio.snapInterval);
                            gui::mainWindow.setsnapIntervalId(msgRadio.snapIntervalId);
                            gui::mainWindow.setdeempId(msgRadio.deempId);
                            gui::mainWindow.setnbEnabled(msgRadio.nbEnabled);
                            gui::mainWindow.setnbLevel(msgRadio.nbLevel);
                            gui::mainWindow.setCMO_BBand(msgRadio.baseband_band);
                            gui::mainWindow.setsquelchEnabled(msgRadio.squelchEnabled);
                            gui::mainWindow.setsquelchLevel(msgRadio.squelchLevel);
                            gui::mainWindow.setFMIFNREnabled(msgRadio.FMIFNREnabled);
                            gui::mainWindow.setfmIFPresetId(msgRadio.fmIFPresetId);
                            gui::mainWindow.setlowPass(msgRadio._lowPass);
                            gui::mainWindow.sethighPass(msgRadio._highPass);
                            gui::mainWindow.setagcAttack(msgRadio.agcAttack);
                            gui::mainWindow.setagcDecay(msgRadio.agcDecay);
                            gui::mainWindow.setcarrierAgc(msgRadio.carrierAgc);
                            gui::mainWindow.setUpdateMenuRcv2Radio(true);
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_RECORD && SrvStatus == ARM_STATUS_FULL_CONTROL)
                    { // RADIO
                        memcpy(&msgRecord, &ibuf[sizeof(InfoHeader)], count);
                        if (msgRecord.UpdateMenu)
                        {
                            // flog::info("PACKET_TYPE_RECORD. RCV.  msgRecord.UpdateMenu {0}, msgRecord.recording {1}", msgRecord.UpdateMenu, msgRecord.recording);
                            // gui::mainWindow.setRecording(msgRecord.recording);
                            // gui::mainWindow.setUpdateMenuRcv3Record(true);
                            /*
                            if (msgRecord.recording == true)
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START, NULL, NULL);
                            else
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMDstopworkerThread, NULL, NULL);
                            */
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_SEARCH)
                    { // SEARCH  && SrvStatus > ARM_STATUS_NOT_CONTROL
                        if (ib_pkt_hdr->sizeOfExtension > 0)
                            count = count - ib_pkt_hdr->sizeOfExtension;
                        memcpy(&msgSearch, &ibuf[sizeof(InfoHeader)], count);
                        if (ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_srch(0))
                        {
                            void *bbufRCV = ::operator new(ib_pkt_hdr->sizeOfExtension);
                            memcpy(bbufRCV, &ibuf[sizeof(InfoHeader) + count], ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setbbuf_srch(bbufRCV, ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setUpdateListRcv5Srch(0, true);
                            ::operator delete(bbufRCV);
                            /*
                            for (int poz = 0; poz < sizeofbbuf; poz = poz + sizeof(fbm)) {
                                memcpy(&fbm, ((void*)bbuf) + poz, sizeof(fbm));
                                std::string listname = std::string(fbm.listName);
                                // strcpy(listname.c_str(), );
                                flog::info("!!!! poz {0}, fbm.listName {1}, listname {1}  ", poz, fbm.listName, listname);
                            }
                            */
                        }
                        if (msgSearch.UpdateMenu)
                        {
                            flog::info("PACKET_TYPE_SEARCH. RCV. msgSearch.UpdateMenu {0}, msgSearch.button_srch {1}, ib_pkt_hdr->sizeOfExtension {2}, msgSearch.statAutoLevel {3}", msgSearch.UpdateMenu, msgSearch.button_srch, ib_pkt_hdr->sizeOfExtension, msgSearch.statAutoLevel);
                            gui::mainWindow.setbutton_srch(0, msgSearch.button_srch);
                            gui::mainWindow.setidOfList_srch(0, msgSearch.idOfList_srch);
                            gui::mainWindow.setLevelDbSrch(0, msgSearch.levelDb);
                            gui::mainWindow.setAuto_levelSrch(0, msgSearch.statAutoLevel);
                            gui::mainWindow.setSNRLevelDb(0, msgSearch.SNRlevelDb);
                            gui::mainWindow.setAKFInd(0, msgSearch.status_AKF);
                            gui::mainWindow.setUpdateModule_srch(0, true);
                            gui::mainWindow.setselectedLogicId(0, msgSearch.selectedLogicId);
                            gui::mainWindow.setUpdateMenuRcv5Srch(true);
                            gui::mainWindow.setUpdateMenuSnd5Srch(0, false);
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_SCAN)
                    { // SEARCH //  && SrvStatus > ARM_STATUS_NOT_CONTROL
                        if (ib_pkt_hdr->sizeOfExtension > 0)
                            count = count - ib_pkt_hdr->sizeOfExtension;
                        // flog::info("PACKET_TYPE_SCAN. 1. count {0}", count);
                        memcpy(&msgScan, &ibuf[sizeof(InfoHeader)], count);
                        if (ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_scan(0))
                        {
                            void *bbufRCV = ::operator new(ib_pkt_hdr->sizeOfExtension);
                            memcpy(bbufRCV, &ibuf[sizeof(InfoHeader) + count], ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setbbuf_scan(bbufRCV, ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setUpdateListRcv6Scan(0, true);
                            ::operator delete(bbufRCV);
                            /*
                            for (int poz = 0; poz < sizeofbbuf; poz = poz + sizeof(fbm)) {
                                memcpy(&fbm, ((void*)bbuf) + poz, sizeof(fbm));
                                std::string listname = std::string(fbm.listName);
                                // strcpy(listname.c_str(), );
                                flog::info("!!!! poz {0}, fbm.listName {1}, listname {1}  ", poz, fbm.listName, listname);
                            }
                            */
                        }
                        gui::mainWindow.setidOfList_scan(0, msgScan.idOfList_scan);
                        if (msgScan.UpdateMenu)
                        {
                            flog::info("PACKET_TYPE_SCAN. RCV. msgScan.UpdateMenu {0}, msgScan.button_scan {1}, ib_pkt_hdr->sizeOfExtension {2}", msgScan.UpdateMenu, msgScan.button_scan, ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setbutton_scan(0, msgScan.button_scan);
                            gui::mainWindow.setAuto_levelScan(0, msgScan.statAutoLevel);
                            gui::mainWindow.setMaxRecWaitTime_scan(0, msgScan.maxRecWaitTime);
                            gui::mainWindow.setMaxRecDuration_scan(0, msgScan.maxRecDuration);
                            // gui::mainWindow.setflag_level_scan(0, msgScan.flag_level);
                            gui::mainWindow.setLevelDbScan(0, msgScan.level);
                            gui::mainWindow.setUpdateModule_scan(0, true);
                            gui::mainWindow.setUpdateMenuRcv6Scan(true);
                            gui::mainWindow.setUpdateMenuSnd6Scan(0, false);
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_CTRL)
                    { // CTRL
                        flog::info("PACKET_TYPE_CTRL. RCV. msgCTRL.UpdateMenu {0}, msgCTRL.button_ctrl {1}, ib_pkt_hdr->sizeOfExtension {2}", msgCTRL.UpdateMenu, msgCTRL.button_ctrl, ib_pkt_hdr->sizeOfExtension);
                        if (ib_pkt_hdr->sizeOfExtension > 0)
                            count = count - ib_pkt_hdr->sizeOfExtension;
                        memcpy(&msgCTRL, &ibuf[sizeof(InfoHeader)], count);
                        if (ib_pkt_hdr->sizeOfExtension > 0) //  && !gui::mainWindow.getbutton_ctrl(0)
                        {
                            void *bbufRCV = ::operator new(ib_pkt_hdr->sizeOfExtension);
                            memcpy(bbufRCV, &ibuf[sizeof(InfoHeader) + count], ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setbbuf_ctrl(bbufRCV, ib_pkt_hdr->sizeOfExtension);
                            ::operator delete(bbufRCV);
                            gui::mainWindow.setUpdateListRcv7Ctrl(0, true);
                            /*
                            for (int poz = 0; poz < sizeofbbuf; poz = poz + sizeof(fbm)) {
                                memcpy(&fbm, ((void*)bbuf) + poz, sizeof(fbm));
                                std::string listname = std::string(fbm.listName);
                                // strcpy(listname.c_str(), );
                                flog::info("!!!! poz {0}, fbm.listName {1}, listname {1}  ", poz, fbm.listName, listname);
                            }
                            */
                        }
                        gui::mainWindow.setidOfList_ctrl(0, msgCTRL.idOfList_ctrl);
                        flog::info("PACKET_TYPE_CTRL.  msgCTRL.idOfList_ctrl {0}", msgCTRL.idOfList_ctrl);
                        if (msgCTRL.UpdateMenu)
                        {
                            flog::info("PACKET_TYPE_CTRL. RCV. msgCTRL.UpdateMenu {0}, msgCTRL.button_ctrl {1}, ib_pkt_hdr->sizeOfExtension {2}", msgCTRL.UpdateMenu, msgCTRL.button_ctrl, ib_pkt_hdr->sizeOfExtension);
                            gui::mainWindow.setbutton_ctrl(0, msgCTRL.button_ctrl);
                            gui::mainWindow.setMaxRecWaitTime_ctrl(0, msgCTRL.maxRecWaitTime);
                            gui::mainWindow.setAuto_levelCtrl(0, msgCTRL.statAutoLevel);
                            gui::mainWindow.setAKFInd_ctrl(0, msgCTRL.status_AKF);
                            gui::mainWindow.setflag_level_ctrl(0, msgCTRL.flag_level);
                            gui::mainWindow.setLevelDbCtrl(0, msgCTRL.level);
                            gui::mainWindow.setUpdateModule_ctrl(0, true);
                            gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                            gui::mainWindow.setUpdateMenuSnd7Ctrl(0, false);
                        }
                    }
                    else if (ib_pkt_hdr->type == PACKET_TYPE_MAIN)
                    { //  && SrvStatus > ARM_STATUS_NOT_CONTROL
                        memcpy(&msgMain, &ibuf[sizeof(InfoHeader)], count);
                        // if (msgStatusServer.id_control == 0)
                        int old_status = gui::mainWindow.getstatusControl(0);
                        int new_status = msgMain.statusServer;
                        // flog::info("TRACE RCV PACKET_TYPE_MAIN old_status {0}, new_status {1}", old_status, new_status);
                        if (new_status != old_status)
                        {
                            gui::mainWindow.setstatusControl(0, msgMain.statusServer);
                            if (new_status == ARM_STATUS_FULL_CONTROL)
                            {
                                /*
                                flog::warn(">>>> GOT FULL_CONTROL! Restarting player to ensure data flow.");
                                // Перезапускаем плеер, чтобы гарантированно активировать всю DSP-цепочку
                                gui::mainWindow.setPlayState(false);
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                gui::mainWindow.setPlayState(true);
                                */
                                flog::info("TRACE RCV PACKET_TYPE_MAIN new_status==ARM_STATUS_FULL_CONTROL ms_interval = {0}", ms_interval);
                                gui::mainWindow.setUpdateMenuSnd(true);
                                gui::mainWindow.setUpdateMenuSnd0Main(0, true);
                                gui::mainWindow.setUpdateMenuSnd2Radio(true);
                                gui::mainWindow.setUpdateMenuSnd3Record(true);
                                gui::mainWindow.setUpdateLists_srch(true);
                                gui::mainWindow.setUpdateMenuSnd5Srch(0, true);
                                gui::mainWindow.setUpdateLists_scan(true);
                                gui::mainWindow.setUpdateMenuSnd6Scan(0, true);
                                gui::mainWindow.setUpdateMenuSnd7Ctrl(0, true);

                                ms_interval = 25; /// 500...1000
                            }
                            else
                            {
                                ms_interval = 750;
                                flog::info("TRACE RCV PACKET_TYPE_MAIN new_status!=ARM_STATUS_FULL_CONTROL ms_interval = {0}", ms_interval);
                            }
                        }
                        // flog::info("TRACE RCV PACKET_TYPE_MAIN UpdateMenu {0}, ib_pkt_hdr->size {1}, msgMain.freq {2}, msgMain.statusServer {3}, status {4}, msgMain.recording {5}, msgMain.scan {6}, idOfList_scan {7} ", msgMain.UpdateMenu, ib_pkt_hdr->size, _freq, msgMain.statusServer, msgMain.status, msgMain.recording, msgMain.scan, msgMain.idOfList_scan);
                        // if (gui::mainWindow.getUpdateMenuSnd0Main(0) == false) {
                        // flog::info("TRACE RCV PACKET_TYPE_MAIN 1");
                        if (msgMain.UpdateMenu)
                        {
                            flog::info("TRACE RCV PACKET_TYPE_MAIN 2 (msgMain.UpdateMenu) msgMain.recording {0}, gui::mainWindow.getRecording() {1}, gui::mainWindow.getUpdateMenuSnd3Record() {2}", msgMain.recording, gui::mainWindow.getRecording(), gui::mainWindow.getUpdateMenuSnd3Record());

                            if (msgMain.recording != gui::mainWindow.getServerRecording(0)) //  .getRecording()
                            {
                                flog::info("TRACE RCV PACKET_TYPE_MAIN 2. if (msgMain.recording != gui::mainWindow.getRecording())");

                                if (!gui::mainWindow.getUpdateMenuSnd3Record())
                                {
                                    if (msgMain.recording)
                                    {
                                        if (!gui::mainWindow.isPlaying())
                                        {
                                            gui::mainWindow.setPlayState(true);
                                        }
                                    }
                                    flog::info("TRACE RCV PACKET_TYPE_MAIN 2. getUpdateMenuSnd3Record {0}", msgMain.recording);
                                    // gui::mainWindow.setRecording(msgMain.recording);
                                    if(msgMain.recording)
                                        gui::mainWindow.setServerRecordingStart(0);
                                    else    
                                        gui::mainWindow.setServerRecordingStop(0);
                                    gui::mainWindow.setUpdateMenuRcv3Record(true);
                                }
                            }
                            /*
                            SinkManager::Stream *_stream = sigpath::sinkManager.getCurrStream("Канал приймання");
                            if (_stream != NULL)
                            {
                                float _level = _stream->getVolume();
                                if (msgMain.level != _level)
                                {
                                    _stream->setVolume(msgMain.level);
                                }
                            }
                            */
                            // gui::mainWindow.setidOfList_srch(0, msgMain.idOfList_srch);
                            flog::info("TRACE RCV PACKET_TYPE_MAIN 3 msgMain.search {0}, gui::mainWindow.getbutton_srch(0) {1}, msgMain.idOfList_srch {2}", msgMain.search, gui::mainWindow.getbutton_srch(0), msgMain.idOfList_srch);
                            if (msgMain.search != gui::mainWindow.getbutton_srch(0))
                            {
                                gui::mainWindow.setbutton_srch(0, msgMain.search);
                                gui::mainWindow.setidOfList_srch(0, msgMain.idOfList_srch);
                                gui::mainWindow.setselectedLogicId(0, msgMain.selectedLogicId);
                                gui::mainWindow.setLevelDbSrch(0, msgMain.setLevelDbSrch);
                                gui::mainWindow.setLevelDbScan(0, msgMain.setLevelDbScan);
                                gui::mainWindow.setLevelDbCtrl(0, msgMain.setLevelDbCtrl);
                                if (msgMain.search)
                                {
                                    if (!gui::mainWindow.isPlaying())
                                    {
                                        gui::mainWindow.setPlayState(true);
                                    }
                                }
                                gui::mainWindow.setUpdateMenuRcv5Srch(true);
                                gui::mainWindow.setUpdateMenuSnd5Srch(0, false);
                            }

                            flog::info("TRACE RCV PACKET_TYPE_MAIN 4 msgMain.scan {0}, gui::mainWindow.getbutton_scan(0) {1}, msgMain.idOfList_scan {2}", msgMain.scan, gui::mainWindow.getbutton_scan(0), msgMain.idOfList_scan);
                            if (msgMain.scan != gui::mainWindow.getbutton_scan(0))
                            {
                                gui::mainWindow.setbutton_scan(0, msgMain.scan);
                                gui::mainWindow.setidOfList_scan(0, msgMain.idOfList_scan);
                                if (msgMain.scan)
                                {
                                    if (!gui::mainWindow.isPlaying())
                                    {
                                        gui::mainWindow.setPlayState(true);
                                    }
                                }
                                gui::mainWindow.setUpdateMenuRcv6Scan(true);
                                gui::mainWindow.setUpdateMenuSnd6Scan(0, false);
                            }
                            flog::info("TRACE RCV PACKET_TYPE_MAIN 5 msgMain.control {0}, gui::mainWindow.getbutton_ctrl(0) {1}, msgMain.idOfList_control {2}", msgMain.control, gui::mainWindow.getbutton_ctrl(0), msgMain.idOfList_control);
                            if (msgMain.control != gui::mainWindow.getbutton_ctrl(0))
                            {
                                gui::mainWindow.setbutton_ctrl(0, msgMain.control);
                                gui::mainWindow.setidOfList_ctrl(0, msgMain.idOfList_control);
                                gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                                gui::mainWindow.setUpdateMenuSnd7Ctrl(0, false);
                            }

                            if (gui::mainWindow.getstatusControl(0) == ARM_STATUS_FULL_CONTROL)
                            {
                                // flog::info("TRACE RCV 1");
                                if (gui::mainWindow.getUpdateMenuSnd0Main(0) == false)
                                {
                                    flog::info("\n      TRACE RCV (msgMain) msgMain.tuningMode {0}, gui::mainWindow.gettuningMode() {1}", msgMain.tuningMode, gui::mainWindow.gettuningMode());
                                    if (msgMain.tuningMode != gui::mainWindow.gettuningMode())
                                    {
                                        gui::mainWindow.settuningMode(msgMain.tuningMode);
                                        core::configManager.acquire();
                                        core::configManager.conf["centerTuning"] = !msgMain.tuningMode;
                                        core::configManager.release(true);
                                        gui::waterfall.VFOMoveSingleClick = msgMain.tuningMode;
                                    }

                                    bool scan_working = false;
                                    if (msgMain.search || msgMain.scan || msgMain.control)
                                    {
                                        scan_working = true;
                                    }
                                    if (!scan_working)
                                    {
                                        double t_freq = msgMain.freq;
                                        double curr_freq = gui::waterfall.getCenterFrequency();
                                        double _offset = sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                                        if (t_freq != curr_freq)
                                        {
                                            // gui::waterfall.setCenterFrequency(t_freq);
                                            tuner::centerTuning(gui::waterfall.selectedVFO, t_freq);
                                            gui::waterfall.centerFreqMoved = true;
                                            usleep(100);
                                        }
                                        if (msgMain.freq == curr_freq && msgMain.offset == _offset)
                                        {
                                            // OK
                                        }
                                        else
                                        {
                                            gui::mainWindow.setUpdateFreq(true);
                                            if (msgMain.tuningMode == tuner::TUNER_MODE_NORMAL)
                                            {
                                                t_freq = msgMain.freq + msgMain.offset;
                                                sigpath::vfoManager.getOffset("Канал приймання");
                                                tuner::tune(msgMain.tuningMode, gui::waterfall.selectedVFO, t_freq);
                                                // flog::info("TUNER_MODE_NORMAL");
                                            }
                                            else
                                            {
                                                tuner::centerTuning(gui::waterfall.selectedVFO, t_freq);
                                                flog::info("TUNER_MODE_CENTER");
                                            }
                                        }
                                        // sigpath::sourceManager.tune(t_freq);

                                        // if (msgMain.tuningMode != _tuningMode) {
                                        flog::info("TRACE RCV (msgMain) msgMain.tuningMode  {0}, _tuningMode {1}, gui::waterfall.VFOMoveSingleClick {2}, playing {3}, t_freq {4}", msgMain.tuningMode, _tuningMode, gui::waterfall.VFOMoveSingleClick, msgMain.playing, t_freq);
                                    }
                                    change_freq = 2;
                                    // gui::mainWindow.settuningMode(msgMain.tuningMode);
                                }
                            }
                        }
                        if (msgMain.playing && !gui::mainWindow.isPlaying())
                        {
                            gui::mainWindow.setPlayState(true);
                        }
                        /* else if (!msgMain.playing && gui::mainWindow.isPlaying()) {
                            gui::mainWindow.setPlayState(false);
                        }
                        */
                    }
                    /*
                    else if (ib_pkt_hdr->type == PACKET_TYPE_STATUS) { // STATUS
                        memcpy(&msgStatusServer, &ibuf[sizeof(InfoHeader)], count);
                        if (msgStatusServer.id_control == 0)
                            gui::mainWindow.setstatusControl(0, msgStatusServer.statusServer);
                        // flog::info("TRACE RCV (msgStatusServer) msgStatusServer.status {0}, msgStatusServer.id_control {1}", msgStatusServer.statusServer, msgStatusServer.id_control);
                    }
                    */
                    changed = false;
                    if (change_freq > 0)
                        change_freq--;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    socket_work = true;

                    if (send_airspy)
                        gui::mainWindow.setUpdateMenuSnd(false);
                    if (send_main)
                    {
                        // gui::mainWindow.setUpdateMenuSnd0Main(0, false);
                        send_main = false;
                    }
                    if (send_radio)
                        gui::mainWindow.setUpdateMenuSnd2Radio(false);
                    if (send_record)
                        gui::mainWindow.setUpdateMenuSnd3Record(false);
                }
                else
                {
                    flog::info("stopworkerThread socket_work {0}", socket_work);
                    if (socket_work)
                    {
                        stopworkerThread = true;
                        break;
                    }
                }
                /*
                if (SrvStatus == ARM_STATUS_FULL_CONTROL) {
                    usleep(100000);
                }
                else {
                    usleep(500000);
                }
                */
            }
        }
        if (ibuf)
        {
            delete[] ibuf;
            ibuf = nullptr; // желательно, чтобы избежать повторного удаления
        }

        if (stopworkerThread)
        {
            flog::info("if (stopworkerThread)");
            gui::mainWindow.setFirstConn(ALL_MOD, true);
            // stop();
            // connInfo->waitForEnd();
            // connInfo->close();
            // if (listener) { listener->close(); }
            // delete this;
        }
    }

    void RemoteRadio::udpSenderThread(const std::string &ip, int port, const std::string &prefix)
    {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
        {
            std::cerr << "[ERROR] socket: " << strerror(errno) << std::endl;
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
        {
            std::cerr << "[ERROR] inet_pton: неверный IP " << ip << std::endl;
            close(sockfd);
            return;
        }

        double frequency = 150000000;
        int bandwidth = 220010;

        while (!stopUdpSender)
        {
            std::time_t unixtime = std::time(nullptr);
            // UDP отправлено: rpm1v.1.2.4;150000000;220010;1749647874;
            std::string rpmprefix = "rpm" + nameInstance + "v." + Version;
            // SrvStatus;
            double _freq = gui::waterfall.getCenterFrequency();
            frequency = _freq + gui::waterfall.getViewOffset();
            bandwidth = gui::mainWindow.getbandwidth();

            std::string message = rpmprefix + ";" +
                                  std::to_string(static_cast<int64_t>(frequency)) + ";" +
                                  std::to_string(bandwidth) + ";" +
                                  std::to_string(unixtime) + ";";

            ssize_t sent = sendto(sockfd, message.c_str(), message.size(), 0,
                                  reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            if (sent < 0)
            {
                std::cerr << "[WARNING] sendto: " << strerror(errno) << std::endl;
            }
            else
            {
                std::cout << "[INFO] UDP отправлено: " << message << std::endl;
            }

            // std::this_thread::sleep_for(std::chrono::seconds(10));
            // спать с прерыванием
            int totalSleep = 0;
            while (totalSleep < 100 && !stopUdpSender)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                totalSleep++;
            }
        }

        close(sockfd);
        std::cout << "[INFO] UDP-поток завершён\n";
    }

    // ИЗМЕНЕНО: ServerHandler теперь очень быстрый (Производитель)
    void RemoteRadio::ServerHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RemoteRadio *_this = (RemoteRadio *)ctx;
        // flog::info(">>>> ServerHandler GOT DATA! Count: {0}", count);
        std::vector<dsp::complex_t> data_copy(data, data + count);
        _this->audioQueue.push(std::move(data_copy));
    }
    /*
    void RemoteRadio::ServerHandler(uint8_t *data, int count, void *ctx)
    {
        // if(!isServer) return;
        // RemoteRadio* _this = (RemoteRadio*)ctx;
        // Write to network

        bb_pkt_hdr->type = PACKET_TYPE_AIRSPY;
        bb_pkt_hdr->size = sizeof(InfoHeader) + count;
        // flog::info(" ServerHandler ...write!!! count {0}. bb_pkt_hdr->size {1}", count, bb_pkt_hdr->size);
        memcpy(&bbuf[sizeof(InfoHeader)], data, count);
        if (conn && conn->isOpen())
        {
            conn->write(bb_pkt_hdr->size, bbuf);
        }
    }
    */
};