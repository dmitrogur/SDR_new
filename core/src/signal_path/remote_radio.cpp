// RemoteRadio.cpp
#include <utils/flog.h>
#include <gui/gui.h>
#include <utils/networking.h>
// #include <server_protocol.h>
#include <memory>

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

#include <pqxx/pqxx> // libpqxx
#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <fstream>
#include <vector>
#include <algorithm>
#include <stdexcept> // Для std::runtime_error

#include "remote_radio.h"

using json = nlohmann::json;

namespace remote
{

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
        int SNRLevelDbSrch = -20;
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
        int SNRLevelDbSrch = -50;
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
    struct SearchModeList
    {
        char listName[32];
        int _mode;
        double _bandwidth;
        double _startFreq;
        double _stopFreq;
        double _interval;
        double _passbandRatio;
        int _tuningTime;   //  = 350;
        bool _status_stop; // = false;
        int _waitingTime;  //  = 1000
        bool _status_record;
        int _lingerTime;    //
        bool _status_ignor; //
        int _level;         //  = -50.0
        bool selected;
        bool _status_direction;
        int _selectedLogicId;
        int _selectedSrchMode;
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
    InfoHeader *bb_pkt_hdr = NULL;
    uint8_t *bb_pkt_data = NULL;

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
        if (listener && !_this->stopAudioSender.load())
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

            _this->stopworkerThread = true;
            _this->socket_work = true;
            if (_this->workerThread.joinable())
            {
                _this->workerThread.join();
            }

            _this->stopworkerThread = false;

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
        stopUdpSender = true;
        stopUdpDBSender = true;
        flog::info("RemoteRadio destructor called.");
        closeProgramm.store(true);
        stop();
        if (bbuf)
        {
            delete[] bbuf;
            bbuf = nullptr;
        }
        if (workerThread.joinable())
        {
            flog::info("Joining workerThread...");
            workerThread.join();
        }
        if (senderUDP.joinable())
        {
            flog::info("Joining senderUDP...");
            senderUDP.join();
        }

        if (senderDbUDP.joinable())
        {
            flog::info("Joining senderDbUDP...");
            senderDbUDP.join();
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

        available = core::configManager.conf.contains("AsterPlus");
        if (available)
        {
            isAsterPlus = core::configManager.conf["AsterPlus"];
        }
        else
        {
            isAsterPlus = false;
            core::configManager.conf["AsterPlus"] = isAsterPlus;
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
        stopUdpDBSender = false;
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
        else
        {
            flog::warn("Error listenerInfo = net::listen {0}:{1}", host, port + 2);
        }

        flog::info("RemoteRadio::start() called.");
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            flog::warn("RemoteRadio::start() called, but it is already running.");
            return;
        }

        flog::info("RemoteRadio::start() - First entry. Starting components...");

        audioQueue.reset();

        sigpath::iqFrontEnd.tempStop();
        sigpath::iqFrontEnd.bindIQStream(&inputStream);
        sigpath::iqFrontEnd.tempStart();

        hnd.start();

        stopAudioSender.store(false);
        stopUdpSender = false;
        stopUdpDBSender = false;
        info_udp = 100;

        // isAsterPlus = true;

        int instNum = std::stoi(nameInstance); // "1" -> 1
        int dbPort = 48000 + instNum;          // 48001 .. 48008

        const char *kConnStrAster = "host=192.168.88.10 user=aster password=aster dbname=ASTER connect_timeout=3";
        const char *kConnStrMalva = "host=192.168.30.100 user=aster password=aster dbname=ASTER connect_timeout=3";
        if (radioMode == 0)
        {
            this->kConnStr = kConnStrAster;

            // телеметрия (ARM)
            this->senderUDP = std::thread(&RemoteRadio::udpReceiverWorker,
                                          this,
                                          "192.168.88.11", // ARM
                                          45712,
                                          "rpm1v.1.2.4");

            // DBSEND/DBACK + БД (ARM+, тот же IP что и ARM, но порт 4800X)
            this->senderDbUDP = std::thread(&RemoteRadio::udpDbWorker,
                                            this,
                                            "192.168.88.11",
                                            dbPort);
        }
        else
        {
            this->kConnStr = kConnStrMalva;

            this->senderUDP = std::thread(&RemoteRadio::udpReceiverWorker,
                                          this,
                                          "192.168.30.100",
                                          45712,
                                          "rpm1v.1.2.4");

            this->senderDbUDP = std::thread(&RemoteRadio::udpDbWorker,
                                            this,
                                            "192.168.30.100",
                                            dbPort);
        }

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
        stopAudioSender.store(true);
        stopUdpSender = true;
        stopUdpDBSender = true;
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
    
        if (senderUDP.joinable())
        {
            senderUDP.join();
        }
        if (senderDbUDP.joinable())
        {
            senderDbUDP.join();
        }
        hnd.stop();

        if (_init && sigpath::iqFrontEnd.isInitialized())
        {
            sigpath::iqFrontEnd.unbindIQStream(&inputStream);
        }
        flog::info("RemoteRadio::stop() finished successfully.");
    }

    void RemoteRadio::audioSenderWorker()
    {
        flog::info("IQ sender thread started.");

        while (!stopAudioSender.load())
        {
            // --- ФАЗА 1: ЖДАТЬ КЛИЕНТА ---
            // Используем `while` вместо `if`. Поток будет здесь "висеть",
            // пока `conn` не станет валидным или пока программу не закроют.
            while ((!conn || !conn->isOpen()) && !stopAudioSender.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Если вышли из цикла ожидания, потому что программу закрывают
            if (stopAudioSender.load())
            {
                break;
            }

            flog::info("IQ SENDER: Client is connected. Starting data transmission.");

            // --- ФАЗА 2: ПЕРЕДАЧА ДАННЫХ ---
            std::vector<dsp::complex_t> data_chunk;
            // Цикл, пока соединение живо и программу не закрывают
            while (conn && conn->isOpen() && !stopAudioSender.load())
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
                    // Проверяем состояние сокета СРАЗУ ПОСЛЕ успешной записи.
                    if (!conn->isOpen())
                    {
                        flog::error("IQ SENDER CRITICAL: Connection closed IMMEDIATELY after a successful write!");
                    }
                }
                else
                {
                    if (stopAudioSender.load())
                    {
                        break; // Если остановили, то выходим
                    }
                    // audioQueue.pop() вернула false, значит, была вызвана request_stop().
                    // Это сигнал к завершению потока.
                    // flog::error("IQ SENDER CRITICAL FAILURE: audioQueue.pop() returned FALSE. The value of stopAudioSender is: {0}", stopAudioSender.load());
                    // break; // Выходим из внутреннего цикла отправки
                    continue;
                }
            }
            // ОТЛАДКА: Выясняем, почему именно мы вышли из цикла
            if (!conn)
            {
                flog::warn("IQ SENDER: Loop exited because conn is NULL.");
            }
            else if (!conn->isOpen())
            {
                flog::warn("IQ SENDER: Loop exited because conn->isOpen() is FALSE.");
            }
            else if (stopAudioSender.load())
            {
                flog::warn("IQ SENDER: Loop exited because stopAudioSender is TRUE.");
            }
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
                    bool send_search = false; // NEW
                    bool send_scan = false;   // NEW
                    bool send_ctrl = false;   // NEW

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
                    else if (gui::mainWindow.getUpdateMenuSnd3Record())              // SrvStatus == ARM_STATUS_FULL_CONTROL &&
                    {                                                                // RADIO
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
                    else if (SrvStatus > ARM_STATUS_NOT_CONTROL && gui::mainWindow.getUpdateMenuSnd5Srch(0))
                    {
                        // --- NEW: РПМ -> АРМ: SEARCH CONFIG ---
                        msgSearch.selectedLogicId = gui::mainWindow.getselectedLogicId(0);
                        msgSearch.idOfList_srch = gui::mainWindow.getidOfList_srch(0);
                        msgSearch.button_srch = gui::mainWindow.getbutton_srch(0);
                        msgSearch.levelDb = gui::mainWindow.getLevelDbSrch(0);
                        msgSearch.SNRlevelDb = gui::mainWindow.getSNRLevelDb(0);
                        msgSearch.status_AKF = gui::mainWindow.getAKFInd(0);
                        msgSearch.UpdateModule = gui::mainWindow.getUpdateModule_srch(0);
                        msgSearch.UpdateLists = gui::mainWindow.getUpdateLists_srch();
                        msgSearch.statAutoLevel = gui::mainWindow.getAuto_levelSrch(0);
                        msgSearch.UpdateMenu = true;

                        ib_pkt_hdr->type = PACKET_TYPE_SEARCH;
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgSearch);
                        ib_pkt_hdr->sizeOfExtension = 0;

                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgSearch, sizeof(msgSearch));

                        if (msgSearch.UpdateLists)
                        {
                            int extSize = gui::mainWindow.getsizeOfbbuf_srch();
                            const size_t maxSize = 256 * MAX_STRUCT_SIZE;
                            size_t totalSize = ib_pkt_hdr->size + (size_t)extSize;
                            if (extSize > 0 && totalSize <= maxSize)
                            {
                                ib_pkt_hdr->sizeOfExtension = extSize;
                                memcpy(&ibuf[ib_pkt_hdr->size],
                                       (void *)gui::mainWindow.getbbuf_srch(),
                                       extSize);
                                ib_pkt_hdr->size = (uint32_t)totalSize;
                            }
                            else if (totalSize > maxSize)
                            {
                                flog::error("PACKET_TYPE_SEARCH: totalSize {0} > maxSize {1}, drop extension",
                                            totalSize, maxSize);
                                ib_pkt_hdr->sizeOfExtension = 0;
                            }
                        }

                        flog::info("PACKET_TYPE_SEARCH (RPM->ARM). Send. idOfList {0}, size {1}, ext {2}",
                                   msgSearch.idOfList_srch, ib_pkt_hdr->size, ib_pkt_hdr->sizeOfExtension);

                        send_search = true;
                    }
                    else if (SrvStatus > ARM_STATUS_NOT_CONTROL && gui::mainWindow.getUpdateMenuSnd6Scan(0))
                    {
                        // --- NEW: РПМ -> АРМ: SCAN CONFIG ---
                        msgScan.idOfList_scan = gui::mainWindow.getidOfList_scan(0);
                        msgScan.button_scan = gui::mainWindow.getbutton_scan(0);
                        msgScan.maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_scan(0);
                        msgScan.maxRecDuration = gui::mainWindow.getMaxRecDuration_scan(0);
                        msgScan.flag_level = true;
                        msgScan.level = gui::mainWindow.getLevelDbScan(0);
                        msgScan.statAutoLevel = gui::mainWindow.getAuto_levelScan(0);
                        msgScan.UpdateModule = gui::mainWindow.getUpdateModule_scan(0);
                        msgScan.UpdateLists = gui::mainWindow.getUpdateLists_scan();
                        msgScan.UpdateMenu = true;

                        int extSize = 0;
                        if (msgScan.UpdateLists)
                            extSize = gui::mainWindow.getsizeOfbbuf_scan();

                        const size_t maxSize = 256 * MAX_STRUCT_SIZE;
                        size_t totalSize = sizeof(InfoHeader) + sizeof(msgScan) + (size_t)extSize;
                        if (totalSize > maxSize)
                        {
                            flog::error("PACKET_TYPE_SCAN: totalSize {0} > maxSize {1}, drop packet", totalSize, maxSize);
                            gui::mainWindow.setUpdateMenuSnd6Scan(0, false);
                        }
                        else
                        {
                            ib_pkt_hdr->type = PACKET_TYPE_SCAN;
                            ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgScan);
                            ib_pkt_hdr->sizeOfExtension = 0;

                            memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgScan, sizeof(msgScan));

                            if (msgScan.UpdateLists && extSize > 0)
                            {
                                ib_pkt_hdr->sizeOfExtension = extSize;
                                memcpy(&ibuf[ib_pkt_hdr->size],
                                       (void *)gui::mainWindow.getbbuf_scan(),
                                       extSize);
                                ib_pkt_hdr->size += extSize;
                            }

                            flog::info("PACKET_TYPE_SCAN (RPM->ARM). Send. idOfList {0}, size {1}, ext {2}",
                                       msgScan.idOfList_scan, ib_pkt_hdr->size, ib_pkt_hdr->sizeOfExtension);
                            send_scan = true;
                        }
                    }
                    else if (SrvStatus > ARM_STATUS_NOT_CONTROL && gui::mainWindow.getUpdateMenuSnd7Ctrl(0))
                    {
                        // --- NEW: РПМ -> АРМ: CTRL CONFIG ---
                        msgCTRL.idOfList_ctrl = gui::mainWindow.getidOfList_ctrl(0);
                        msgCTRL.button_ctrl = gui::mainWindow.getbutton_ctrl(0);
                        msgCTRL.flag_level = gui::mainWindow.getflag_level_ctrl(0);
                        msgCTRL.level = gui::mainWindow.getLevelDbCtrl(0);
                        msgCTRL.maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_ctrl(0);
                        msgCTRL.statAutoLevel = gui::mainWindow.getAuto_levelCtrl(0);
                        msgCTRL.status_AKF = gui::mainWindow.getAKFInd_ctrl(0);

                        msgCTRL.UpdateModule = gui::mainWindow.getUpdateModule_ctrl(0);
                        msgCTRL.UpdateMenu = true;

                        // Смотрим на реальный размер буфера, а не на флаг
                        int extSize = gui::mainWindow.getsizeOfbbuf_ctrl();
                        bool haveList = (extSize > 0);
                        msgCTRL.UpdateLists = haveList;

                        ib_pkt_hdr->type = PACKET_TYPE_CTRL;
                        ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgCTRL);
                        ib_pkt_hdr->sizeOfExtension = 0;

                        memcpy(&ibuf[sizeof(InfoHeader)], (uint8_t *)&msgCTRL, sizeof(msgCTRL));

                        if (haveList)
                        {
                            const size_t maxSize = 256 * MAX_STRUCT_SIZE;
                            size_t totalSize = ib_pkt_hdr->size + (size_t)extSize;
                            if (totalSize <= maxSize)
                            {
                                ib_pkt_hdr->sizeOfExtension = extSize;
                                memcpy(&ibuf[ib_pkt_hdr->size],
                                       (void *)gui::mainWindow.getbbuf_ctrl(),
                                       extSize);
                                ib_pkt_hdr->size = (uint32_t)totalSize;
                            }
                            else
                            {
                                flog::error("PACKET_TYPE_CTRL: totalSize {0} > maxSize {1}, drop extension",
                                            totalSize, maxSize);
                                ib_pkt_hdr->sizeOfExtension = 0;
                            }
                        }

                        flog::info("PACKET_TYPE_CTRL (RPM->ARM). Send. idOfList {0}, size {1}, ext {2}",
                                   msgCTRL.idOfList_ctrl, ib_pkt_hdr->size, ib_pkt_hdr->sizeOfExtension);

                        bool flagUpdate = gui::mainWindow.getUpdateListRcv7Ctrl(0);

                        flog::info("RPM CTRL SEND: UpdateMenuSnd7Ctrl={0}, UpdateListRcv7Ctrl(0)={1}, extSize={2}",
                                   gui::mainWindow.getUpdateMenuSnd7Ctrl(0), flagUpdate, extSize);
                        send_ctrl = true;
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
                    {
                        // 1. Сначала вычисляем размер "полезной нагрузки" без расширения
                        // count - это общий размер данных пакета (без заголовка InfoHeader)
                        int msgSize = count;
                        if (ib_pkt_hdr->sizeOfExtension > 0)
                        {
                            msgSize = count - ib_pkt_hdr->sizeOfExtension;
                        }

                        // Защита: не копируем, если пришло меньше, чем размер структуры
                        if (msgSize < sizeof(msgScan))
                        {
                            flog::error("Received SCAN packet too small: {0} < {1}", msgSize, sizeof(msgScan));
                        }
                        else
                        {
                            memcpy(&msgScan, &ibuf[sizeof(InfoHeader)], sizeof(msgScan));
                        }

                        // 2. Обработка расширения (Списки частот)
                        if (ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_scan(0))
                        {
                            // Проверка на адекватность размера
                            if (ib_pkt_hdr->sizeOfExtension > 2000000)
                            { // 2 MB limit
                                flog::error("Invalid extension size: {0}", ib_pkt_hdr->sizeOfExtension);
                            }
                            else
                            {
                                // Используем вектор для безопасного копирования
                                try
                                {
                                    std::vector<uint8_t> tempBuf(ib_pkt_hdr->sizeOfExtension);

                                    // ВАЖНО: Смещение = Заголовок + Структура Настроек
                                    // Это надежнее, чем использовать переменную count
                                    size_t dataOffset = sizeof(InfoHeader) + sizeof(msgScan);

                                    // Копируем данные из ibuf во временный вектор
                                    memcpy(tempBuf.data(), &ibuf[dataOffset], ib_pkt_hdr->sizeOfExtension);

                                    // Передаем в GUI (там должно происходить копирование внутрь)
                                    gui::mainWindow.setbbuf_scan(tempBuf.data(), ib_pkt_hdr->sizeOfExtension);

                                    // Сигнализируем, что данные приняты
                                    gui::mainWindow.setUpdateListRcv6Scan(0, true);

                                    // tempBuf уничтожится здесь, но setbbuf_scan должен был скопировать данные
                                }
                                catch (const std::exception &e)
                                {
                                    flog::error("Receiver allocation error: {0}", e.what());
                                }
                            }
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
                    /*
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
                        }
                        gui::mainWindow.setidOfList_ctrl(0, msgCTRL.idOfList_ctrl);
                        flog::info("PACKET_TYPE_CTRL.  msgCTRL.idOfList_ctrl {0}", msgCTRL.idOfList_ctrl);
                        if (msgCTRL.UpdateMenu)
                        {
                            flog::info("PACKET_TYPE_CTRL. RCV. msgCTRL.UpdateMenu {0}, msgCTRL.button_ctrl {1}, msgCTRL.level {2}", msgCTRL.UpdateMenu, msgCTRL.button_ctrl, msgCTRL.level);
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
                    */
                    else if (ib_pkt_hdr->type == PACKET_TYPE_CTRL)
                    { // CTRL
                        // ВАЖНО: отделяем "состояние" от "банка частот".
                        // Банк обновляем только если модуль Спостереження/CTRL не запущен на РПМ.
                        bool ctrl_running = gui::mainWindow.getbutton_ctrl(0);

                        // count = общий размер "тела" без InfoHeader
                        int msgSize = count;
                        if (ib_pkt_hdr->sizeOfExtension > 0)
                            msgSize -= ib_pkt_hdr->sizeOfExtension;

                        if (msgSize != (int)sizeof(msgCTRL))
                        {
                            flog::error("PACKET_TYPE_CTRL: invalid msg size: {0} != {1}",
                                        msgSize, sizeof(msgCTRL));
                        }
                        else
                        {
                            memcpy(&msgCTRL, &ibuf[sizeof(InfoHeader)], msgSize);

                            flog::info(
                                "PACKET_TYPE_CTRL. RCV. UpdateMenu={0}, button_ctrl={1}, "
                                "ext={2}, ctrl_running={3}, idOfList={4}",
                                msgCTRL.UpdateMenu,
                                msgCTRL.button_ctrl,
                                ib_pkt_hdr->sizeOfExtension,
                                ctrl_running,
                                msgCTRL.idOfList_ctrl);

                            // --- ОБНОВЛЕНИЕ БАНКА ЧАСТОТ (extension) ---
                            if (ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_ctrl(0))
                            {
                                if (!ctrl_running)
                                {
                                    void *bbufRCV = ::operator new(ib_pkt_hdr->sizeOfExtension);
                                    memcpy(bbufRCV,
                                           &ibuf[sizeof(InfoHeader) + msgSize],
                                           ib_pkt_hdr->sizeOfExtension);

                                    gui::mainWindow.setbbuf_ctrl(bbufRCV, ib_pkt_hdr->sizeOfExtension);
                                    ::operator delete(bbufRCV);

                                    gui::mainWindow.setUpdateListRcv7Ctrl(0, true);

                                    flog::info(
                                        "PACKET_TYPE_CTRL: bank updated, extSize={0}",
                                        ib_pkt_hdr->sizeOfExtension);
                                }
                                else
                                {
                                    // Модуль уже работает — банк не трогаем
                                    flog::warn(
                                        "PACKET_TYPE_CTRL: got bank update while CTRL/"
                                        "Supervisor is running -> dropping extension (size={0})",
                                        ib_pkt_hdr->sizeOfExtension);
                                }
                            }

                            // --- ОБНОВЛЕНИЕ ВЫБОРА СПИСКА И НАСТРОЕК ---
                            gui::mainWindow.setidOfList_ctrl(0, msgCTRL.idOfList_ctrl);
                            flog::info("PACKET_TYPE_CTRL. msgCTRL.idOfList_ctrl {0}",
                                       msgCTRL.idOfList_ctrl);

                            if (msgCTRL.UpdateMenu)
                            {
                                flog::info(
                                    "PACKET_TYPE_CTRL. RCV. msgCTRL.UpdateMenu {0}, "
                                    "msgCTRL.button_ctrl {1}, msgCTRL.level {2}",
                                    msgCTRL.UpdateMenu,
                                    msgCTRL.button_ctrl,
                                    msgCTRL.level);

                                gui::mainWindow.setbutton_ctrl(0, msgCTRL.button_ctrl);
                                gui::mainWindow.setMaxRecWaitTime_ctrl(0,
                                                                       msgCTRL.maxRecWaitTime);
                                gui::mainWindow.setAuto_levelCtrl(0, msgCTRL.statAutoLevel);
                                gui::mainWindow.setAKFInd_ctrl(0, msgCTRL.status_AKF);
                                gui::mainWindow.setflag_level_ctrl(0, msgCTRL.flag_level);
                                gui::mainWindow.setLevelDbCtrl(0, msgCTRL.level);
                                gui::mainWindow.setUpdateModule_ctrl(0, true);
                                gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                                gui::mainWindow.setUpdateMenuSnd7Ctrl(0, false);
                            }
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
                                    if (msgMain.recording)
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
                                gui::mainWindow.setLevelDbSrch(0, msgMain.setLevelDbSrch);
                                gui::mainWindow.setselectedLogicId(0, msgMain.selectedLogicId);
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
                                gui::mainWindow.setLevelDbScan(0, msgMain.setLevelDbScan);

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
                                gui::mainWindow.setLevelDbCtrl(0, msgMain.setLevelDbCtrl);
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
                        send_main = false;
                    if (send_radio)
                        gui::mainWindow.setUpdateMenuSnd2Radio(false);
                    if (send_record)
                        gui::mainWindow.setUpdateMenuSnd3Record(false);
                    if (send_search)
                    {
                        gui::mainWindow.setUpdateMenuSnd5Srch(0, false);
                        send_search = false;
                    }
                    if (send_scan)
                    {
                        gui::mainWindow.setUpdateMenuSnd6Scan(0, false);
                        send_scan = false;
                    }
                    if (send_ctrl)
                    {
                        gui::mainWindow.setUpdateMenuSnd7Ctrl(0, false);
                        send_ctrl = false;
                    }
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

    //=========================================================================================
    //=========================================================================================
    //=========================================================================================
    struct DbStats
    {
        uint64_t tx_telemetry = 0;  // скольких телеметрий отправлено
        uint64_t rx_dbsend = 0;     // получено DBSEND
        uint64_t tx_dback = 0;      // отправлено DBACK
        uint64_t parse_err = 0;     // ошибок парсинга DBSEND
        uint64_t addr_mismatch = 0; // не наш instance
        uint32_t last_task_id = 0;  // последний task_id
        int last_work = 0;          // последний work_status
        uint32_t last_cmd_ts = 0;   // unixtime последней команды
        sockaddr_in last_peer{};    // кому отвечали DBACK
    };
    /*
        void RemoteRadio::processObservationMode(uint32_t task_id)
        {
            std::cout << "[DB] Начало обработки режима НАБЛЮДЕНИЕ (4) для задачи " << task_id << std::endl;
            pqxx::connection conn{this->kConnStr};
            pqxx::work txn{conn};

            // TODO: Для наблюдения, вероятно, потребуется более сложный запрос,
            // чтобы получить dopinfo, scard, и другие специфичные поля для каждого канала.
            pqxx::result task_result = txn.exec_params(
                "SELECT b.nazv AS bank_name, f.freq1 AS freq, f.band AS bandwidth, f.modul as mode, f.dopinfo, f.scard " // Пример
                "FROM public.task AS t "
                "LEFT JOIN public.bank AS b ON b.id = t.id_bank AND b.del = 'N' "
                "LEFT JOIN public.freq AS f ON f.id_bank = b.id "
                "WHERE t.id = $1 ORDER BY f.freq1;",
                task_id);

            if (task_result.empty())
                throw std::runtime_error("Задача не найдена или не содержит частот.");

            json config = {
                {"bookmarkDisplayMode", 1}, {"selectedList", "General"}, {"status_AKF", true}};

            json bookmarks = json::object();
            int channel_num = 1;
            for (const auto &row : task_result)
            {
                if (!row["freq"].is_null())
                {
                    std::string channel_id = "C" + std::to_string(channel_num++);
                    bookmarks[channel_id] = {
                        {"frequency", row["freq"].as<double>()},
                        {"bandwidth", row["bandwidth"].as<double>(12500.0)},
                        {"mode", row["mode"].as<int>(0)},
                        {"dopinfo", row["dopinfo"].as<std::string>(channel_id)},          // Используем ID как доп.инфо
                        {"scard", row["scard"].as<std::string>("vsink_1_" + channel_id)}, // Генерируем scard
                        {"level", -20},
                        {"Signal", 0}};
                }
            }

            config["lists"]["General"] = {
                {"bookmarks", bookmarks},
                {"flaglevel", true},
                {"genlevel", -20},
                {"showOnWaterfall", true}};

            txn.commit();
            saveConfigToFile(config);
            std::cout << "[DB] Конфигурация для режима НАБЛЮДЕНИЕ успешно создана.\n";
        }
    */
    void RemoteRadio::saveConfigToFile(const json &config, const std::string &filename)
    {
        std::ofstream o(filename);
        if (!o.is_open())
        {
            throw std::runtime_error("Не удалось открыть файл для записи: " + filename);
        }
        o << std::setw(4) << config << std::endl;
        o.close();
    }
    // ===============================================================================================
    using WFSkipFoundBookmark = ImGui::WaterFall::SkipFoundBookmark;

    static bool isGeneralBankRow(const pqxx::row &row)
    {
        try
        {
            // вариант: bank как число (1 == General)
            try
            {
                int b = row["bank"].as<int>(-1);
                if (b == 1)
                    return true;
                if (b != -1)
                    return false;
            }
            catch (...)
            {
            }

            // вариант: bank как строка ("General")
            try
            {
                std::string bstr = row["bank"].as<std::string>("");
                if (bstr == "General" || bstr == "GENERAL" || bstr == "general")
                    return true;
                if (!bstr.empty())
                    return false;
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }

        // если поля bank нет — считаем, что подходит
        return true;
    }

    static bool buildBlacklistFromDb(const std::string &connStr, int instNum)
    {
        try
        {
            pqxx::connection conn{connStr};
            pqxx::work txn{conn};

            auto r2 = txn.exec_params(
                "SELECT id, num_instance, freq_bl, modul_bl, signal_bl "
                "FROM public.black_list "
                "WHERE num_instance = $1 "
                "ORDER BY freq_bl;",
                instNum);

            flog::info("[ARM+] buildBlacklistFromDb: instNum={0}, rows={1}", instNum, r2.size());

            std::vector<WFSkipFoundBookmark> items;
            items.reserve(r2.size());

            for (const auto &row : r2)
            {
                WFSkipFoundBookmark bi{};
                bi.frequency = 0.0;
                bi.bandwidth = 12500.0f; // дефолт для Search
                bi.mode = 0;
                bi.level = -50;
                bi.selected = false;
                bi.ftime = std::time(nullptr);

                // freq_bl -> frequency
                if (!row["freq_bl"].is_null())
                {
                    try
                    {
                        long long f = row["freq_bl"].as<long long>(0);
                        if (f > 0)
                            bi.frequency = static_cast<double>(f);
                    }
                    catch (...)
                    {
                        std::cerr << "[DB] freq_bl: cannot convert to long long\n";
                    }
                }

                // modul_bl -> mode
                if (!row["modul_bl"].is_null())
                {
                    std::string modulStr;
                    try
                    {
                        modulStr = row["modul_bl"].as<std::string>("");
                    }
                    catch (...)
                    {
                        modulStr.clear();
                    }

                    if (!modulStr.empty())
                    {
                        int modeIndex = bi.mode;
                        bool setByNumber = false;

                        // 1) число 0..7
                        try
                        {
                            size_t pos = 0;
                            int m = std::stoi(modulStr, &pos);
                            if (pos == modulStr.size() && m >= 0 && m < 8)
                            {
                                modeIndex = m;
                                setByNumber = true;
                            }
                        }
                        catch (...)
                        {
                        }

                        // 2) строка
                        if (!setByNumber)
                        {
                            static const char *demodModeListFile[] = {
                                "ЧМ",
                                "ЧМ-Ш",
                                "AM",
                                "ПБС",
                                "ВБС",
                                "HC",
                                "НБС",
                                "CMO"};

                            for (int i = 0; i < 8; ++i)
                            {
                                if (modulStr == demodModeListFile[i])
                                {
                                    modeIndex = i;
                                    break;
                                }
                            }
                        }

                        bi.mode = modeIndex;
                    }
                }

                // signal_bl -> level
                if (!row["signal_bl"].is_null())
                {
                    try
                    {
                        std::string sigStr = row["signal_bl"].as<std::string>("");
                        if (!sigStr.empty())
                            bi.level = std::stoi(sigStr);
                    }
                    catch (...)
                    {
                        std::cerr << "[DB] signal_bl: cannot convert to int, use -50\n";
                        bi.level = -50;
                    }
                }

                items.push_back(std::move(bi));
            }

            txn.commit();

            // Вот здесь — правильный вызов:
            gui::waterfall.UpdateBlackList(items);

            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[DB] buildBlacklistFromDb exception: " << e.what() << "\n";
            return false;
        }
    }

    //---------------------------------------------------------------------------------------------
    static bool buildSearchStructFromDb(const pqxx::result &freqRows)
    {
        flog::info("[ARM+] buildSearchStructFromDb: START, freqRows.size={0}", freqRows.size());

        if (freqRows.empty())
        {
            flog::warn("[ARM+] buildSearchStructFromDb: freqRows.empty()");
            return false;
        }

        // --- Ищем строку банка General в freqRows (одну) ---
        const pqxx::row *rowGeneral = nullptr;
        for (const auto &r : freqRows)
        {
            if (isGeneralBankRow(r))
            {
                rowGeneral = &r;
                break;
            }
        }

        if (!rowGeneral)
        {
            flog::warn("[ARM+] buildSearchStructFromDb: no row for bank=General in freqRows");
            return false;
        }
        const auto &row = *rowGeneral;

        // -------- парсим поля из БД (freq, freq2, band, step, modul) --------
        SearchModeList fbm{};
        long long dbFreq = 0;
        long long dbFreq2 = 0;
        long long dbBand = 0;
        long long dbStep = 0;
        std::string dbModul;

        // freq
        if (!row["freq"].is_null())
        {
            try
            {
                dbFreq = row["freq"].as<long long>(0);
            }
            catch (...)
            {
            }
        }

        // freq2
        if (!row["freq2"].is_null())
        {
            try
            {
                dbFreq2 = row["freq2"].as<long long>(0);
            }
            catch (...)
            {
            }
        }

        // band
        bool bandFromDb = false;
        if (!row["band"].is_null())
        {
            try
            {
                dbBand = row["band"].as<long long>(0);
                if (dbBand > 0)
                    bandFromDb = true;
            }
            catch (...)
            {
                try
                {
                    std::string bwStr = row["band"].as<std::string>("");
                    if (!bwStr.empty())
                    {
                        double bw = std::stod(bwStr);
                        if (bw > 0.0)
                        {
                            dbBand = (long long)bw;
                            bandFromDb = true;
                        }
                    }
                }
                catch (...)
                {
                }
            }
        }

        // step
        bool stepFromDb = false;
        if (!row["step"].is_null())
        {
            try
            {
                dbStep = row["step"].as<long long>(0);
                if (dbStep > 0)
                    stepFromDb = true;
            }
            catch (...)
            {
            }
        }

        // modul
        if (!row["modul"].is_null())
        {
            try
            {
                dbModul = row["modul"].as<std::string>("");
            }
            catch (...)
            {
            }
        }

        // =========================================================
        // 1) Берём текущий буфер поиска с РПМ
        // =========================================================
        void *srcBuf = gui::mainWindow.getbbuf_srch();
        int bufSize = gui::mainWindow.getsizeOfbbuf_srch();

        flog::info("[ARM+] buildSearchStructFromDb: initial srcBuf={0}, bufSize={1}, sizeof(SearchModeList)={2}",
                   (const void *)srcBuf, bufSize, (int)sizeof(SearchModeList));

        // ждём до ~5 секунд пока GUI заполнит bbuf_srch
        for (int i = 0; i < 50; ++i)
        {
            srcBuf = gui::mainWindow.getbbuf_srch();
            bufSize = gui::mainWindow.getsizeOfbbuf_srch();
            if (srcBuf && bufSize > 0 && bufSize % (int)sizeof(SearchModeList) == 0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const int sizeofList = (int)sizeof(SearchModeList);
        bool invalidBuf = (!srcBuf || bufSize <= 0 || (bufSize % sizeofList) != 0);

        // =========================================================
        // ВЕТКА A: буфер битый / пустой → создаём НОВЫЙ только с General
        // =========================================================
        if (invalidBuf)
        {
            flog::warn("[ARM+] buildSearchStructFromDb: invalid buffer, recreate with single 'General' list. "
                       "srcBuf={0}, bufSize={1}, sizeof={2}",
                       (const void *)srcBuf, bufSize, sizeofList);

            int newSize = sizeofList;
            void *bbufNew = ::operator new((size_t)newSize);
            SearchModeList *arr = static_cast<SearchModeList *>(bbufNew);

            SearchModeList gen{};
            std::snprintf(gen.listName, sizeof(gen.listName), "%s", "General");

            // Применяем значения из БД
            if (dbFreq > 0)
                gen._startFreq = static_cast<double>(dbFreq);
            if (dbFreq2 > 0)
                gen._stopFreq = static_cast<double>(dbFreq2);
            if (bandFromDb && dbBand > 0)
                gen._bandwidth = static_cast<double>(dbBand);
            if (stepFromDb && dbStep > 0)
                gen._interval = static_cast<double>(dbStep);

            // _mode по modul
            if (!dbModul.empty())
            {
                int modeIndex = gen._mode; // текущее
                bool setByNumber = false;

                // 1) modul как число
                try
                {
                    size_t pos = 0;
                    int m = std::stoi(dbModul, &pos);
                    if (pos == dbModul.size() && m >= 0 && m < 8)
                    {
                        modeIndex = m;
                        setByNumber = true;
                    }
                }
                catch (...)
                {
                }

                // 2) modul как строка
                if (!setByNumber)
                {
                    static const char *demodModeListFile[] = {
                        "ЧМ",
                        "ЧМ-Ш",
                        "AM",
                        "ПБС",
                        "ВБС",
                        "HC",
                        "НБС",
                        "CMO"};

                    for (int i = 0; i < 8; ++i)
                    {
                        if (dbModul == demodModeListFile[i])
                        {
                            modeIndex = i;
                            break;
                        }
                    }
                }

                gen._mode = modeIndex;
            }

            *arr = gen;

            // Передаём в GUI
            gui::mainWindow.setbbuf_srch(bbufNew, newSize);
            ::operator delete(bbufNew);

            // НАСТРОЙКА ПОИСКА — General (он один, индекс 0)
            gui::mainWindow.setidOfList_srch(0, 0);
            // gui::mainWindow.setLevelDbSrch(0, -77);
            gui::mainWindow.setAuto_levelSrch(0, true);
            gui::mainWindow.setAKFInd(0, true);
            gui::mainWindow.setUpdateModule_srch(0, true);
            gui::mainWindow.setUpdateListRcv5Srch(0, true);
            gui::mainWindow.setUpdateMenuRcv5Srch(true);
            gui::mainWindow.setUpdateMenuSnd5Srch(0, true);
            gui::mainWindow.setbutton_srch(0, true);

            flog::info("[ARM+] buildSearchStructFromDb: DONE (recreated buffer with single 'General')");
            gui::mainWindow.setUpdateLists_srch(true); // есть новый список
            return true;
        }

        // =========================================================
        // ВЕТКА B: буфер валиден → сохраняем все списки, правим/добавляем только General
        // =========================================================

        // Делаем копию, чтобы править
        void *bbufCopy = ::operator new((size_t)bufSize);
        std::memcpy(bbufCopy, srcBuf, (size_t)bufSize);

        int nLists = bufSize / sizeofList;
        int generalIdx = -1;

        // --- 1) Пытаемся найти существующий General и обновить его ---
        for (int idx = 0; idx < nLists; ++idx)
        {
            SearchModeList local{};
            std::memcpy(&local,
                        static_cast<uint8_t *>(bbufCopy) + idx * sizeofList,
                        sizeofList);

            std::string name(local.listName);
            flog::info("[ARM+] buildSearchStructFromDb: list[{0}] name='{1}'", idx, name);

            if (name == "General")
            {
                generalIdx = idx;

                // применяем параметры из БД к этому SearchModeList
                if (dbFreq > 0)
                    local._startFreq = static_cast<double>(dbFreq);

                if (dbFreq2 > 0)
                    local._stopFreq = static_cast<double>(dbFreq2);

                if (bandFromDb && dbBand > 0)
                    local._bandwidth = static_cast<double>(dbBand);

                if (stepFromDb && dbStep > 0)
                    local._interval = static_cast<double>(dbStep);

                if (!dbModul.empty())
                {
                    int modeIndex = local._mode;
                    bool setByNumber = false;

                    // modul как число
                    try
                    {
                        size_t pos = 0;
                        int m = std::stoi(dbModul, &pos);
                        if (pos == dbModul.size() && m >= 0 && m < 8)
                        {
                            modeIndex = m;
                            setByNumber = true;
                        }
                    }
                    catch (...)
                    {
                    }

                    // modul как строка
                    if (!setByNumber)
                    {
                        static const char *demodModeListFile[] = {
                            "ЧМ",
                            "ЧМ-Ш",
                            "AM",
                            "ПБС",
                            "ВБС",
                            "HC",
                            "НБС",
                            "CMO"};

                        for (int i = 0; i < 8; ++i)
                        {
                            if (dbModul == demodModeListFile[i])
                            {
                                modeIndex = i;
                                break;
                            }
                        }
                    }

                    local._mode = modeIndex;
                }

                // Записываем обратно
                std::memcpy(static_cast<uint8_t *>(bbufCopy) + idx * sizeofList,
                            &local,
                            sizeofList);

                flog::info("[ARM+] buildSearchStructFromDb: updated existing 'General' at idx={0}", idx);
                break;
            }
        }

        // --- 2) Если General не нашли — ДОБАВЛЯЕМ его в конец массива ---
        if (generalIdx < 0)
        {
            flog::info("[ARM+] buildSearchStructFromDb: 'General' not found, append new");

            int newSize = bufSize + sizeofList;
            void *bbufNew = ::operator new((size_t)newSize);

            // копируем старые списки
            std::memcpy(bbufNew, bbufCopy, (size_t)bufSize);
            ::operator delete(bbufCopy);

            // новый General
            SearchModeList gen{};
            std::snprintf(gen.listName, sizeof(gen.listName), "%s", "General");

            if (dbFreq > 0)
                gen._startFreq = static_cast<double>(dbFreq);
            if (dbFreq2 > 0)
                gen._stopFreq = static_cast<double>(dbFreq2);
            if (bandFromDb && dbBand > 0)
                gen._bandwidth = static_cast<double>(dbBand);
            if (stepFromDb && dbStep > 0)
                gen._interval = static_cast<double>(dbStep);

            if (!dbModul.empty())
            {
                int modeIndex = gen._mode;
                bool setByNumber = false;

                try
                {
                    size_t pos = 0;
                    int m = std::stoi(dbModul, &pos);
                    if (pos == dbModul.size() && m >= 0 && m < 8)
                    {
                        modeIndex = m;
                        setByNumber = true;
                    }
                }
                catch (...)
                {
                }

                if (!setByNumber)
                {
                    static const char *demodModeListFile[] = {
                        "ЧМ",
                        "ЧМ-Ш",
                        "AM",
                        "ПБС",
                        "ВБС",
                        "HC",
                        "НБС",
                        "CMO"};

                    for (int i = 0; i < 8; ++i)
                    {
                        if (dbModul == demodModeListFile[i])
                        {
                            modeIndex = i;
                            break;
                        }
                    }
                }

                gen._mode = modeIndex;
            }

            // кладём новый General в конец
            std::memcpy(static_cast<uint8_t *>(bbufNew) + bufSize,
                        &gen,
                        sizeofList);

            generalIdx = nLists; // новый — последний

            // отдаём новый буфер
            gui::mainWindow.setbbuf_srch(bbufNew, newSize);
            ::operator delete(bbufNew);

            // НАСТРОЙКА ПОИСКА
            gui::mainWindow.setidOfList_srch(0, generalIdx);
            // gui::mainWindow.setLevelDbSrch(0, -77);
            gui::mainWindow.setAuto_levelSrch(0, true);
            gui::mainWindow.setAKFInd(0, true);
            gui::mainWindow.setUpdateModule_srch(0, true);
            gui::mainWindow.setUpdateListRcv5Srch(0, true);
            gui::mainWindow.setUpdateMenuRcv5Srch(true);
            gui::mainWindow.setUpdateMenuSnd5Srch(0, true);
            gui::mainWindow.setbutton_srch(0, true);

            flog::info("[ARM+] buildSearchStructFromDb: DONE, appended new 'General', idx={0}", generalIdx);
            gui::mainWindow.setUpdateLists_srch(true);
            return true;
        }

        // --- 3) General найден и обновлён в bbufCopy ---
        gui::mainWindow.setbbuf_srch(bbufCopy, bufSize);
        ::operator delete(bbufCopy);

        gui::mainWindow.setidOfList_srch(0, generalIdx);
        // gui::mainWindow.setLevelDbSrch(0, -77);
        gui::mainWindow.setAuto_levelSrch(0, true);
        gui::mainWindow.setAKFInd(0, true);
        gui::mainWindow.setUpdateModule_srch(0, true);
        gui::mainWindow.setUpdateListRcv5Srch(0, true);
        gui::mainWindow.setUpdateMenuRcv5Srch(true);
        gui::mainWindow.setUpdateMenuSnd5Srch(0, true);
        gui::mainWindow.setbutton_srch(0, true);

        flog::info("[ARM+] buildSearchStructFromDb: DONE, updated existing 'General', idx={0}", generalIdx);
        gui::mainWindow.setUpdateLists_srch(true);
        return true;
    }

#pragma pack(push, 1)
    struct CompactBookmarkData
    {
        int id;
        double freq;
        float bw;
        int mode;
        int level;
        int signal;
    };
#pragma pack(pop)

    static std::string safe_get_str(const pqxx::row &row, const char *col)
    {
        try
        {
            if (row[col].is_null())
                return "";
            return row[col].as<std::string>();
        }
        catch (...)
        {
            return "";
        }
    }

    static bool buildScanStructFromDb(const pqxx::result &freqRows)
    {
        flog::info("[ARM+] buildScanStructFromDb: START, freqRows.size={0}", freqRows.size());

        if (freqRows.empty())
        {
            flog::warn("[ARM+] buildScanStructFromDb: freqRows.empty()");
            return false;
        }

        // 1) Берём исходный буфер от РПМ (списки сканирования)
        void *srcBuf = gui::mainWindow.getbbuf_scan();
        int bufSize = gui::mainWindow.getsizeOfbbuf_scan();

        flog::info("[ARM+] buildScanStructFromDb: initial srcBuf={0}, bufSize={1}",
                   (const void *)srcBuf, bufSize);

        // Подождём до ~5 секунд, если буфер ещё не готов
        if (!srcBuf || bufSize <= 0)
        {
            for (int i = 0; i < 50; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                srcBuf = gui::mainWindow.getbbuf_scan();
                bufSize = gui::mainWindow.getsizeOfbbuf_scan();
                flog::info("[ARM+] buildScanStructFromDb: wait buf, try={0}, srcBuf={1}, bufSize={2}",
                           i, (const void *)srcBuf, bufSize);
                if (srcBuf && bufSize > 0)
                    break;
            }
        }

        if (!srcBuf || bufSize <= 0)
        {
            flog::warn("[ARM+] buildScanStructFromDb: invalid buffer after wait: srcBuf={0}, bufSize={1}",
                       (const void *)srcBuf, bufSize);
            return false;
        }

        // 2) Делаем копию буфера, чтобы править её
        void *bbufCopy = ::operator new((size_t)bufSize);
        std::memcpy(bbufCopy, srcBuf, (size_t)bufSize);
        uint8_t *buf = static_cast<uint8_t *>(bbufCopy);

        flog::info("[ARM+] buildScanStructFromDb: using srcBuf copy={0}, bufSize={1}",
                   (const void *)buf, bufSize);

        // 3) Парсим списки, ищем General
        size_t offset = 0;
        int listIndex = 0;
        int generalListIdx = -1;
        size_t generalItemsOffset = 0;
        size_t generalCountOffset = 0;
        int oldCount = 0;
        bool parseError = false;

        flog::info("[ARM+] buildScanStructFromDb: parse lists, totalBufSize={0}", bufSize);

        while (offset + 32 + sizeof(int) <= (size_t)bufSize)
        {
            // --- читаем имя списка (32 байта) ---
            char nameBuf[32];
            std::memcpy(nameBuf, buf + offset, 32);
            offset += 32;

            nameBuf[31] = '\0';
            std::string listName(nameBuf, strnlen(nameBuf, 32));

            // --- читаем count ---
            if (offset + sizeof(int) > (size_t)bufSize)
            {
                flog::warn("[ARM+] buildScanStructFromDb: not enough space for count in list {0}", listIndex);
                parseError = true;
                break;
            }

            int count = 0;
            size_t countOffset = offset;
            std::memcpy(&count, buf + offset, sizeof(int));
            offset += sizeof(int);

            flog::info("[ARM+] buildScanStructFromDb: listIndex={0}, name='{1}', count={2}, offset={3}",
                       listIndex, listName, count, offset);

            if (count < 0)
            {
                flog::warn("[ARM+] buildScanStructFromDb: negative count in list {0}", listIndex);
                parseError = true;
                break;
            }

            // Сколько байт занимают элементы
            size_t structSize = sizeof(CompactBookmarkData);
            size_t itemsBytes = (size_t)count * structSize;

            // Проверка, что элементы не выходят за буфер
            if (offset + itemsBytes > (size_t)bufSize)
            {
                flog::warn("[ARM+] buildScanStructFromDb: list {0} items out of buffer: offset={1}, itemsBytes={2}, bufSize={3}",
                           listIndex, offset, itemsBytes, bufSize);
                parseError = true;
                break;
            }

            if (listName == "General")
            {
                generalListIdx = listIndex;
                generalItemsOffset = offset;
                generalCountOffset = countOffset;
                oldCount = count;

                flog::info("[ARM+] buildScanStructFromDb: FOUND 'General': listIndex={0}, itemsOffset={1}, countOffset={2}, oldCount={3}",
                           listIndex, generalItemsOffset, generalCountOffset, oldCount);

                // --- наш диагностический блок ---
                size_t requiredBytes = (size_t)oldCount * structSize;
                size_t blockStart = generalItemsOffset;
                size_t blockEnd = generalItemsOffset + requiredBytes;

                flog::error("===== DEBUG STRUCT ANALYSIS =====");
                flog::error("sizeof(CompactBookmarkData) = {0}", structSize);
                flog::error("General.count (oldCount) = {0}", oldCount);
                flog::error("Required bytes to hold items = {0}", requiredBytes);
                flog::error("General block begin offset = {0}", blockStart);
                flog::error("General block end offset   = {0}", blockEnd);
                flog::error("Full buffer size (bufSize)= {0}", bufSize);

                if (blockEnd > (size_t)bufSize)
                {
                    flog::error("!!! FATAL: ITEMS BLOCK EXCEEDS BUFFER SIZE !!!");
                    parseError = true;
                }
                else
                {
                    flog::info("Items block fits inside buffer. Safe to continue.");
                }

                break;
            }

            // Пропускаем элементы этого списка
            offset += itemsBytes;
            ++listIndex;
        }

        // Если парсинг сломан → ничего не трогаем, выходим
        if (parseError)
        {
            flog::warn("[ARM+] buildScanStructFromDb: parseError=true, exit without modification");
            ::operator delete(bbufCopy);
            return false;
        }

        if (generalListIdx < 0)
        {
            flog::warn("[ARM+] buildScanStructFromDb: 'General' list not found in scan buffer");
            ::operator delete(bbufCopy);
            return false;
        }

        // 4) Готовим область для General
        uint8_t *itemsBase = buf + generalItemsOffset;
        int maxItems = oldCount;

        if (maxItems <= 0)
        {
            flog::warn("[ARM+] buildScanStructFromDb: General has non-positive count={0}", maxItems);
            ::operator delete(bbufCopy);
            return false;
        }

        // Жёсткая защита: не больше MAX_COUNT_OF_DATA
        if (maxItems > MAX_COUNT_OF_DATA)
        {
            flog::warn("[ARM+] buildScanStructFromDb: General oldCount={0} > MAX_COUNT_OF_DATA={1}, clamp",
                       maxItems, MAX_COUNT_OF_DATA);
            maxItems = MAX_COUNT_OF_DATA;
        }

        // 5) Заполняем General по данным из БД
        int used = 0;
        const size_t structSize = sizeof(CompactBookmarkData);

        for (std::size_t iRow = 0; iRow < freqRows.size() && used < maxItems; ++iRow)
        {
            const auto &row = freqRows[iRow];

            if (!isGeneralBankRow(row))
            {
                flog::info("[ARM+] buildScanStructFromDb: row[{0}] skipped (not General)", iRow);
                continue;
            }

            flog::info("[ARM+] buildScanStructFromDb: row[{0}] → item[{1}] BEGIN", iRow, used);

            CompactBookmarkData item{};
            item.id = used + 1;

            // -------- freq --------
            {
                std::string fs = safe_get_str(row, "freq");
                double freq = 0.0;
                if (!fs.empty() && fs != "-")
                {
                    try
                    {
                        freq = std::stod(fs);
                    }
                    catch (...)
                    {
                        freq = 0.0;
                    }
                }
                item.freq = freq;
            }

            // -------- bw --------
            {
                std::string bs = safe_get_str(row, "band");
                float bw = 250000.0f;
                if (!bs.empty() && bs != "-")
                {
                    try
                    {
                        double bwd = std::stod(bs);
                        if (bwd > 0.0)
                            bw = static_cast<float>(bwd);
                    }
                    catch (...)
                    {
                        // оставляем дефолт
                    }
                }
                item.bw = bw;
            }

            // -------- mode (пока 0, как раньше) --------
            {
                std::string ms = safe_get_str(row, "modul");
                int mode = 0;
                // при необходимости сюда можно добавить логику из АРМ
                item.mode = mode;
            }

            // -------- level --------
            {
                item.level = -50; // фикс для сканирования
            }

            // -------- signal (DMR / ТЛФ / Авто) --------
            {
                std::string ss = safe_get_str(row, "signal"); // "DMR_V", "DMR", ...

                int sig = 0;
                if (!ss.empty())
                {
                    std::string s = ss;

                    // режем пробелы по краям
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                        s.pop_back();
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                        s.erase(s.begin());

                    // латиницу в верхний регистр
                    for (auto &c : s)
                        c = (char)std::toupper((unsigned char)c);

                    if (s.rfind("DMR", 0) == 0)
                    {
                        sig = 2;
                    }
                    else if (s == "ТЛФ")
                    {
                        sig = 1;
                    }
                    else
                    {
                        sig = 0;
                    }
                }

                flog::info("[ARM+] buildScanStructFromDb: SIGNAL parsed: raw='{0}' → sig={1}", ss, sig);
                item.signal = sig;
            }

            // --- ЖЁСТКАЯ ПРОВЕРКА ПЕРЕД memcpy ---
            size_t dstOffset = generalItemsOffset + (size_t)used * structSize;
            if (dstOffset + structSize > (size_t)bufSize)
            {
                flog::error("[ARM+] buildScanStructFromDb: dstOffset out of range: dstOffset={0}, structSize={1}, bufSize={2}",
                            dstOffset, structSize, bufSize);
                parseError = true;
                break;
            }

            flog::info("[ARM+] buildScanStructFromDb: item[{0}] => id={1}, freq={2}, bw={3}, mode={4}, level={5}, signal={6}",
                       used, item.id, item.freq, item.bw, item.mode, item.level, item.signal);

            std::memcpy(buf + dstOffset, &item, structSize);

            flog::info("[ARM+] buildScanStructFromDb: memcpy done for item[{0}], destOffset={1}, size={2}",
                       used, dstOffset, (int)structSize);

            ++used;
            flog::info("[ARM+] buildScanStructFromDb: row[{0}] → item[{1}] END", iRow, used - 1);
        }

        if (parseError)
        {
            flog::warn("[ARM+] buildScanStructFromDb: parseError after fill items, exit without applying");
            ::operator delete(bbufCopy);
            return false;
        }

        // 6) Обнуляем хвост, если used < maxItems
        if (used < maxItems)
        {
            flog::info("[ARM+] buildScanStructFromDb: zero tail from {0} to {1}", used, maxItems);
            CompactBookmarkData zero{};
            for (int j = used; j < maxItems; ++j)
            {
                size_t dstOffset = generalItemsOffset + (size_t)j * structSize;
                if (dstOffset + structSize > (size_t)bufSize)
                {
                    flog::error("[ARM+] buildScanStructFromDb: zero-tail dstOffset out of range: j={0}, dstOffset={1}, bufSize={2}",
                                j, dstOffset, bufSize);
                    break;
                }
                std::memcpy(buf + dstOffset, &zero, structSize);
            }
        }

        // 7) Обновляем count (используем фактическое used)
        std::memcpy(buf + generalCountOffset, &used, sizeof(int));
        flog::info("[ARM+] buildScanStructFromDb: updated count at offset={0}, used={1}",
                   generalCountOffset, used);

        // 8) Отдаём изменённый буфер в GUI
        flog::info("[ARM+] buildScanStructFromDb: setbbuf_scan ptr={0}, size={1}",
                   (const void *)bbufCopy, bufSize);
        gui::mainWindow.setbbuf_scan(bbufCopy, bufSize);
        flog::info("[ARM+] buildScanStructFromDb: setbbuf_scan DONE");

        // Как и раньше у тебя: GUI копирует данные, поэтому можем удалить локальный буфер
        ::operator delete(bbufCopy);
        flog::info("[ARM+] buildScanStructFromDb: buffer deleted locally");

        // 9) Настраиваем сканер на список General
        flog::info("[ARM+] buildScanStructFromDb: setidOfList_scan server=0, listIdx={0}", generalListIdx);
        gui::mainWindow.setidOfList_scan(0, generalListIdx);
        gui::mainWindow.setAuto_levelScan(0, true);
        // gui::mainWindow.setMaxRecWaitTime_scan(0, 10);
        // gui::mainWindow.setMaxRecDuration_scan(0, 8);
        // gui::mainWindow.setLevelDbScan(0, -50);

        gui::mainWindow.setbutton_scan(0, true);
        gui::mainWindow.setUpdateModule_scan(0, true);

        // Сказать сканеру перечитать bbuf_scan и обновить lists
        gui::mainWindow.setUpdateListRcv6Scan(0, true);
        gui::mainWindow.setUpdateMenuRcv6Scan(true);
        gui::mainWindow.setUpdateMenuSnd6Scan(0, false);

        flog::info("[ARM+] buildScanStructFromDb: DONE, used={0}, listIdx={1}", used, generalListIdx);
        gui::mainWindow.setUpdateLists_scan(true);      // есть новый список
        gui::mainWindow.setUpdateMenuSnd6Scan(0, true); // РПМ -> АРМ: PACKET_TYPE_SCAN

        return true;
    }

    //---------------------------------------------------------------------------------------------
    struct CtrlModeList
    {
        char listName[32];
        int sizeOfList;
        int bookmarkName[MAX_COUNT_OF_CTRL_LIST];
        double frequency[MAX_COUNT_OF_CTRL_LIST];
        float bandwidth[MAX_COUNT_OF_CTRL_LIST];
        int mode[MAX_COUNT_OF_CTRL_LIST];
        int level[MAX_COUNT_OF_CTRL_LIST];
        char scard[32];
        int Signal[MAX_COUNT_OF_CTRL_LIST];
    };

    static bool buildControlStructFromDb(const pqxx::result &freqRows)
    {
        flog::info("[ARM+] buildControlStructFromDb: START, freqRows.size={0}",
                   freqRows.size());

        if (freqRows.empty())
        {
            flog::warn("[ARM+] buildControlStructFromDb: freqRows.empty()");
            return false;
        }

        CtrlModeList general{};
        std::snprintf(general.listName, sizeof(general.listName), "%s",
                      "General");

        int idx = 0;
        for (std::size_t i = 0;
             i < freqRows.size() && idx < MAX_COUNT_OF_CTRL_LIST;
             ++i)
        {
            const auto &row = freqRows[i];

            if (!isGeneralBankRow(row))
                continue;

            general.bookmarkName[idx] = idx + 1;

            // --- freq (freq1 AS freq) ---
            {
                std::string fs = safe_get_str(row, "freq");
                double freq = 0.0;
                if (!fs.empty() && fs != "-")
                {
                    try
                    {
                        freq = std::stod(fs);
                    }
                    catch (...)
                    {
                        freq = 0.0;
                    }
                }
                general.frequency[idx] = freq;
            }

            // --- bandwidth (band) ---
            {
                std::string bs = safe_get_str(row, "band");
                double bw = 12500.0;
                if (!bs.empty() && bs != "-")
                {
                    try
                    {
                        bw = std::stod(bs);
                    }
                    catch (...)
                    {
                        bw = 12500.0;
                    }
                }
                general.bandwidth[idx] = static_cast<float>(bw);
            }

            // --- mode ---
            {
                std::string ms = safe_get_str(row, "modul");
                int mode = 0;
                if (ms == "DMR")
                    mode = 2;
                else if (ms == "AM")
                    mode = 1;
                general.mode[idx] = mode;
            }

            // --- level ---
            general.level[idx] = -76;

            // --- Signal: "DMR*", "ТЛФ", "Авто/остальное" ---
            {
                std::string ss = safe_get_str(row, "signal");
                int sig = 0;

                if (!ss.empty())
                {
                    std::string s = ss;

                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                        s.pop_back();
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                        s.erase(s.begin());

                    for (auto &c : s)
                        c = (char)std::toupper((unsigned char)c);

                    if (s.rfind("DMR", 0) == 0)
                        sig = 2;
                    else if (s == "ТЛФ")
                        sig = 1;
                    else
                        sig = 0;
                }

                general.Signal[idx] = sig;
            }

            ++idx;
        }

        general.sizeOfList = idx;

        if (idx == 0)
        {
            flog::warn("[ARM+] buildControlStructFromDb: General from DB "
                       "has 0 items");
            return false;
        }
        /*
        for (int i = 0; i < general.sizeOfList; ++i)
        {
            flog::info("[ARM+] CTRL General{0}: freq={1} band={2} mode={3} "
                       "sig={4}",
                       i, general.frequency[i], general.bandwidth[i],
                       general.mode[i], general.Signal[i]);
        }
        */

        void *srcBuf = gui::mainWindow.getbbuf_ctrl();
        int bufSize = gui::mainWindow.getsizeOfbbuf_ctrl();

        flog::info("[ARM+] buildControlStructFromDb: initial srcBuf={0}, "
                   "bufSize={1}, sizeof(CtrlModeList)={2}",
                   (const void *)srcBuf, bufSize,
                   (int)sizeof(CtrlModeList));

        const int listSize = (int)sizeof(CtrlModeList);
        bool bufOk = (srcBuf != nullptr && bufSize > 0 && (bufSize % listSize) == 0);

        if (!bufOk)
        {
            flog::warn("[ARM+] buildControlStructFromDb: invalid or empty "
                       "bbuf_ctrl, create buffer with only 'General'");

            const int newSize = listSize;
            void *bbuf = ::operator new(
                static_cast<std::size_t>(newSize));
            std::memcpy(bbuf, &general, sizeof(CtrlModeList));

            gui::mainWindow.setbbuf_ctrl(bbuf, newSize);
            ::operator delete(bbuf);

            gui::mainWindow.setidOfList_ctrl(0, 0);
            gui::mainWindow.setAuto_levelCtrl(0, true);
            gui::mainWindow.setAKFInd_ctrl(0, true);
            gui::mainWindow.setflag_level_ctrl(0, true);
            gui::mainWindow.setbutton_ctrl(0, true);
            gui::mainWindow.setUpdateModule_ctrl(0, true);

            gui::mainWindow.setUpdateListRcv7Ctrl(0, true);
            gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
            gui::mainWindow.setUpdateMenuSnd7Ctrl(0, true);

            flog::info("[ARM+] buildControlStructFromDb: DONE "
                       "(recreated buffer with only General), items={0}",
                       idx);
            return true;
        }

        void *bbufCopy = ::operator new(
            static_cast<std::size_t>(bufSize));
        std::memcpy(bbufCopy, srcBuf, (std::size_t)bufSize);

        int nLists = bufSize / listSize;
        int generalIndex = -1;

        for (int i = 0; i < nLists; ++i)
        {
            CtrlModeList tmp{};
            std::memcpy(&tmp,
                        static_cast<uint8_t *>(bbufCopy) + i * listSize,
                        sizeof(CtrlModeList));

            std::string name(tmp.listName,
                             strnlen(tmp.listName,
                                     sizeof(tmp.listName)));
            flog::info("[ARM+] buildControlStructFromDb: listIndex={0}, "
                       "name='{1}', sizeOfList={2}",
                       i, name, tmp.sizeOfList);

            if (name == "General")
                generalIndex = i;
        }

        void *finalBuf = nullptr;
        int finalSize = 0;

        if (generalIndex >= 0)
        {
            flog::info("[ARM+] buildControlStructFromDb: replace existing "
                       "'General' at index {0}",
                       generalIndex);

            std::memcpy(static_cast<uint8_t *>(bbufCopy) + generalIndex * listSize,
                        &general,
                        sizeof(CtrlModeList));

            finalBuf = bbufCopy;
            finalSize = bufSize;
        }
        else
        {
            flog::info("[ARM+] buildControlStructFromDb: 'General' not "
                       "found, append as new list");

            const int newSize = bufSize + listSize;
            void *bbufNew = ::operator new(
                static_cast<std::size_t>(newSize));

            std::memcpy(bbufNew, bbufCopy, (std::size_t)bufSize);
            std::memcpy(static_cast<uint8_t *>(bbufNew) + bufSize,
                        &general,
                        sizeof(CtrlModeList));

            ::operator delete(bbufCopy);

            finalBuf = bbufNew;
            finalSize = newSize;
            generalIndex = nLists;
        }

        gui::mainWindow.setbbuf_ctrl(finalBuf, finalSize);
        ::operator delete(finalBuf);

        gui::mainWindow.setidOfList_ctrl(0, generalIndex);
        gui::mainWindow.setAuto_levelCtrl(0, true);
        gui::mainWindow.setAKFInd_ctrl(0, true);
        gui::mainWindow.setflag_level_ctrl(0, true);
        gui::mainWindow.setbutton_ctrl(0, true);
        gui::mainWindow.setUpdateModule_ctrl(0, true);

        gui::mainWindow.setUpdateListRcv7Ctrl(0, true);
        gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
        gui::mainWindow.setUpdateMenuSnd7Ctrl(0, true);

        flog::info("[ARM+] buildControlStructFromDb: DONE, used {0} rows "
                   "(General), generalIndex={1}, finalSize={2}",
                   idx, generalIndex, finalSize);

        return true;
    }

    void RemoteRadio::udpDbWorker(const std::string &ip, int port)
    {
        flog::info("udpDbWorker START (ip={0}, port={1})", ip, port);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (!isAsterPlus)
        {
            flog::warn("udpDbWorker: isAsterPlus == false, exit");
            return;
        }

        int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
        {
            flog::warn("udpDbWorker socket error: {0}", strerror(errno));
            return;
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = INADDR_ANY;

        int reuse = 1;
        ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif

        if (::bind(sockfd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
        {
            flog::warn("udpDbWorker bind {0}:<<{1}>> error: {2}", ip, port, strerror(errno));
            ::close(sockfd);
            return;
        }

        sockaddr_in arm{};
        arm.sin_family = AF_INET;
        arm.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &arm.sin_addr) <= 0)
        {
            flog::warn("udpDbWorker inet_pton error, ip={0}", ip);
            ::close(sockfd);
            return;
        }

        std::string instanceStr = "rpm" + nameInstance;
        flog::warn("[UDP] DB worker started, bind ok :{0} ({1})", port, instanceStr);

        int fl = ::fcntl(sockfd, F_GETFL, 0);
        if (fl != -1)
            ::fcntl(sockfd, F_SETFL, fl | O_NONBLOCK);

        auto cooldownUntil = std::chrono::steady_clock::time_point::min();
        char ack[256]{};
        int acklen = 0;

        while (!stopUdpDBSender)
        {
            auto nowLoop = std::chrono::steady_clock::now();
            if (nowLoop < cooldownUntil)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            for (;;)
            {
                sockaddr_in peer{};
                socklen_t plen = sizeof(peer);
                char buf[1024];

                ssize_t n = ::recvfrom(sockfd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&peer), &plen);
                if (n < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;

                    flog::warn("udpDbWorker recvfrom error: {0}", strerror(errno));
                    break;
                }

                buf[n] = '\0';
                std::cout << "[RX-DB] " << buf << "  from " << ::inet_ntoa(peer.sin_addr)
                          << ":" << ntohs(peer.sin_port) << "\n";

                char inst[128] = {0};
                char s_unixt[32] = {0};
                char s_task[32] = {0};
                char s_ws[32] = {0};

                int matched = ::sscanf(buf, "DBSEND;%127[^;];%31[^;];%31[^;];%31[^;]", inst, s_unixt, s_task, s_ws);
                if (matched != 4)
                    continue;
                if (instanceStr != inst)
                    continue;

                cooldownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);

                uint32_t unixtime = static_cast<uint32_t>(::strtoul(s_unixt, nullptr, 10));
                uint32_t task_id = static_cast<uint32_t>(::strtoul(s_task, nullptr, 10));
                int ws = static_cast<int>(::strtol(s_ws, nullptr, 10));
                if (ws < 0 || ws > 5)
                    ws = 0;

                currentTaskId.store(task_id, std::memory_order_relaxed);

                if (ws == 0)
                {
                    flog::info("[ARM+] DBSEND: ws=0 -> stop all modes locally, no DB query. inst={0}, task_id={1}",
                               instanceStr, task_id);

                    gui::mainWindow.setbutton_srch(0, false);
                    gui::mainWindow.setUpdateModule_srch(0, true);
                    gui::mainWindow.setUpdateMenuRcv5Srch(true);
                    gui::mainWindow.setUpdateMenuSnd5Srch(0, true);

                    gui::mainWindow.setbutton_scan(0, false);
                    gui::mainWindow.setUpdateModule_scan(0, true);
                    gui::mainWindow.setUpdateMenuRcv6Scan(true);
                    gui::mainWindow.setUpdateMenuSnd6Scan(0, true);

                    gui::mainWindow.setbutton_ctrl(0, false);
                    gui::mainWindow.setUpdateModule_ctrl(0, true);
                    gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(0, true);
                }
                else
                {
                    bool canStartDb =
                        !dbInFlight.load(std::memory_order_relaxed) || lastDbTaskId != task_id;

                    if (canStartDb)
                    {
                        dbInFlight.store(true, std::memory_order_relaxed);
                        lastDbTaskId = task_id;
                        std::string connStr = this->kConnStr;

                        std::thread([this, task_id, connStr]
                                    {
                        try
                        {
                            flog::warn("udpDbWorker DB thread start, task_id={0}", task_id);
                            pqxx::connection conn{connStr};
                            if (!conn.is_open())
                                throw std::runtime_error("[DB] cannot open connection");

                            try
                            {
                                conn.prepare("q",
                                    "SELECT t.id AS task_id, "
                                    "       b.nazv AS bank_name, "
                                    "       f.freq1 AS freq, "
                                    "       f.freq2 AS freq2, "
                                    "       modul, band, signal, step, b.regim "
                                    "FROM public.task AS t "
                                    "LEFT JOIN public.bank AS b "
                                    "       ON b.id = t.id_bank "
                                    "      AND b.del = 'N' "
                                    "LEFT JOIN public.freq AS f "
                                    "       ON f.id_bank = b.id "
                                    "WHERE t.id = $1 "
                                    "ORDER BY t.id, f.freq1;");
                            }
                            catch (const pqxx::usage_error &)
                            {
                            }

                            pqxx::work txn{conn};
                            auto r1 = txn.exec_prepared("q", task_id);

                            flog::info("[DB] r1 rows={0}", r1.size());

                            for (std::size_t i = 0; i < r1.size(); ++i)
                            {
                                const auto &row = r1[i];
                                auto safe_get = [&](const char *col) -> std::string
                                {
                                    try
                                    {
                                        if (row[col].is_null())
                                            return "-";
                                        return row[col].as<std::string>();
                                    }
                                    catch (const std::exception &e)
                                    {
                                        return std::string("<err: ") + e.what() + ">";
                                    }
                                    catch (...)
                                    {
                                        return "<err>";
                                    }
                                };

                                std::cout << "[DB] row[" << i << "] task_id=" << safe_get("task_id")
                                          << " regim=" << safe_get("regim")
                                          << " bank_name=" << safe_get("bank_name")
                                          << " freq=" << safe_get("freq")
                                          << " freq2=" << safe_get("freq2")
                                          << " modul=" << safe_get("modul")
                                          << " band=" << safe_get("band")
                                          << " signal=" << safe_get("signal")
                                          << " step=" << safe_get("step") << std::endl;
                            }

                            std::string bankName;
                            std::string regim;

                            if (!r1.empty())
                            {
                                bankName = r1[0]["bank_name"].is_null()
                                           ? "General"
                                           : r1[0]["bank_name"].as<std::string>("");
                                if (!r1[0]["regim"].is_null())
                                    regim = r1[0]["regim"].as<std::string>("");
                            }
                            else
                            {
                                flog::info("udpDbWorker: ERROR r1.empty(), task_id={0}", task_id);
                            }

                            int ws_local = -1;
                            if (regim == "Спостереження")
                                ws_local = 3;
                            else if (regim == "Сканування")
                                ws_local = 2;
                            else if (regim == "Пошук")
                                ws_local = 1;
                            else if (regim == "Ручний")
                                ws_local = 0;

                            if (ws_local == 1)
                            {
                                int instNum = std::stoi(this->nameInstance);
                                bool blOk = buildBlacklistFromDb(connStr, instNum);
                                bool srchOk = buildSearchStructFromDb(r1);
                                flog::info("[ARM+] DBSEND search: blOk={0}, srchOk={1}", blOk, srchOk);
                            }
                            else if (ws_local == 2)
                            {
                                bool scOk = buildScanStructFromDb(r1);
                                flog::info("[ARM+] Scan: scOk={0}", scOk);
                            }
                            else if (ws_local == 3)
                            {
                                bool ctOk = buildControlStructFromDb(r1);
                                flog::info("[ARM+] Control: ctOk={0}", ctOk);
                            }

                            txn.commit();
                        }
                        catch (const pqxx::broken_connection &e)
                        {
                            flog::warn("[DB] broken_connection: {0}", e.what());
                        }
                        catch (const std::exception &e)
                        {
                            flog::warn("[DB] exception: {0}", e.what());
                        }

                        this->dbInFlight.store(false, std::memory_order_relaxed); })
                            .detach();

                        // ---------- ТУТ ДОБАВЛЕН DBASK ----------
                        char ask[256];
                        int asklen = std::snprintf(
                            ask, sizeof(ask),
                            "DBASK;%s;%u;%u;%d;",
                            instanceStr.c_str(), unixtime, task_id, ws);
                        flog::info("[ACK IDLE] {0}", ack);
                        /*                                
                        if (asklen > 0)
                        {
                            // 1) На порт, с которого пришёл DBSEND (peer)
                            int r1 = ::sendto(
                                sockfd, ask, (size_t)asklen, 0,
                                reinterpret_cast<const sockaddr *>(&peer),
                                sizeof(peer));
                            if (r1 < 0)
                                flog::warn("[UDP] DBASK sendto(peer) failed: {0}", strerror(errno));

                            // 2) На порт 45712 ARM+
                            sockaddr_in armMain{};
                            armMain.sin_family = AF_INET;
                            armMain.sin_port = htons(45712);
                            armMain.sin_addr = peer.sin_addr;

                            int r2 = ::sendto(
                                sockfd, ask, (size_t)asklen, 0,
                                reinterpret_cast<const sockaddr *>(&armMain),
                                sizeof(armMain));
                            if (r2 < 0)
                                flog::warn("[UDP] DBASK sendto(45712) failed: {0}", strerror(errno));
                            else
                                flog::info("[UDP] DBASK sent: {0}", ask);
                        }
                        */
                        // ----------------------------------------
                    }
                }

                /*
                acklen = ::snprintf(ack, sizeof(ack), "DBACK;%s;%u;%u;%d;", instanceStr.c_str(), unixtime, task_id, ws);
            
                if (acklen > 0)
                {
                    int r = ::sendto(sockfd, ack, (size_t)acklen, 0,
                                     reinterpret_cast<const sockaddr *>(&arm),
                                     sizeof(arm));
                    if (r < 0)
                        flog::warn("DBACK sendto(ARM+) error: {0}", strerror(errno));
                    else
                        flog::info("[ACK->ARM+] {0}:{1} \"{2}\"", ip, port, ack);
                }
                */
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        ::close(sockfd);
        flog::info("udpDbWorker STOP");
    }

    //===========================================================================================
    void RemoteRadio::udpReceiverWorker(const std::string &ip, int port, const std::string &prefix)
    {
        flog::info("udpReceiverWorker (telemetry) START (ip={0}, port={1})", ip, port);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // ---- один-единственный UDP-сокет для ТЕЛЕМЕТРИИ ----
        int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
        {
            std::cerr << "[ERROR] socket: " << strerror(errno) << "\n";
            return;
        }

        // привязываем источник к 0.0.0.0:port (для телеметрии это 45712)
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = INADDR_ANY;

        int reuse = 1;
        ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif

        if (::bind(sockfd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
        {
            std::cerr << "[ERROR] bind " << ip.c_str() << ":<<" << port << ">>: " << strerror(errno) << "\n";
            ::close(sockfd);
            return;
        }

        // адрес ARM (получатель телеметрии)
        sockaddr_in arm{};
        arm.sin_family = AF_INET;
        arm.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &arm.sin_addr) <= 0)
        {
            std::cerr << "[ERROR] inet_pton: " << ip << "\n";
            ::close(sockfd);
            return;
        }

        // instance / версия
        std::string instanceStr = "rpm" + nameInstance;       // "rpm1"
        std::string rpmprefix = instanceStr + "v." + Version; // "rpm1v.1.2.4"

        flog::warn("[UDP] telemetry started, bind ok :{0} ({1})", port, instanceStr);

        // неблокирующий сокет нам тут уже не нужен для recvfrom — мы его вообще не вызываем
        // но на всякий случай оставим неблокирующий режим, вреда нет
        int fl = ::fcntl(sockfd, F_GETFL, 0);
        if (fl != -1)
            ::fcntl(sockfd, F_SETFL, fl | O_NONBLOCK);

        // тик телеметрии
        const auto tick = std::chrono::milliseconds(100);
        auto next_tick = std::chrono::steady_clock::now() + tick;

        while (!stopUdpSender)
        {
            // 1) тик телеметрии по расписанию
            auto now = std::chrono::steady_clock::now();
            if (now >= next_tick)
            {
                // --- собираем актуальные поля ---
                double _freq = gui::waterfall.getCenterFrequency();
                double _offset = 0.0;
                int _tuningMode = gui::mainWindow.gettuningMode();

                if (_tuningMode != tuner::TUNER_MODE_CENTER)
                {
                    std::string nameVFO = "Канал приймання";
                    auto it = gui::waterfall.vfos.find(nameVFO);
                    if (it != gui::waterfall.vfos.end())
                        _offset = it->second->generalOffset;
                }

                double frequency = _freq + _offset;
                int bandwidth = gui::mainWindow.getbandwidth();
                int demod_id = gui::mainWindow.getselectedDemodID();
                bool isPlaying = gui::mainWindow.isPlaying();
                bool recording = gui::mainWindow.getRecording();
                bool search = gui::mainWindow.getbutton_srch(0);
                bool scan = gui::mainWindow.getbutton_scan(0);
                bool control = gui::mainWindow.getbutton_ctrl(0);

                int work_status = 0;
                if (control)
                    work_status = 4;
                else if (scan)
                    work_status = 3;
                else if (search)
                    work_status = 2;
                else if (recording)
                    work_status = 1;

                int ws_sent = work_status; // currentWorkStatus.load(...) при желании
                unsigned task_sent = static_cast<unsigned>(currentTaskId.load(std::memory_order_relaxed));

                // --- проверяем, изменилось ли что-то КРОМЕ unixtime ---
                bool changed =
                    !prevInit ||
                    frequency != prevFrequency ||
                    bandwidth != prevBandwidth ||
                    demod_id != prevDemodId ||
                    isPlaying != prevIsPlaying ||
                    ws_sent != prevWorkStatus ||
                    task_sent != prevTaskId;

                auto now2 = std::chrono::steady_clock::now();
                auto diff = now2 - lastSend;

                // раз в FORCE_PERIOD (10 сек) — принудительная отправка
                bool forceSend = (diff >= FORCE_PERIOD);
                // не чаще раза в секунду даже при изменениях
                bool canSendNow = (diff >= std::chrono::seconds(1));

                if (changed && !canSendNow)
                    changed = false;

                bool needTelemetry = changed || forceSend;

                if (needTelemetry)
                {
                    uint32_t unixtime = static_cast<uint32_t>(::time(nullptr));
                    char telemetry[512];

                    int len = ::snprintf(telemetry,
                                         sizeof(telemetry),
                                         "%s;%lld;%d;%u;%d;%d;%d;%u;",
                                         rpmprefix.c_str(), // rpm1v.1.2.4
                                         static_cast<long long>(frequency),
                                         static_cast<int>(bandwidth),
                                         static_cast<unsigned>(unixtime),
                                         static_cast<int>(demod_id),
                                         isPlaying ? 1 : 0,
                                         ws_sent,
                                         task_sent);

                    if (len > 0)
                    {
                        ssize_t sent = ::sendto(sockfd,
                                                telemetry,
                                                static_cast<size_t>(len),
                                                0,
                                                reinterpret_cast<const sockaddr *>(&arm),
                                                sizeof(arm));
                        if (sent < 0)
                        {
                            std::cerr << "[WARNING] sendto (telemetry): " << strerror(errno) << std::endl;
                        }
                        else
                        {
                            if (info_udp >= 0 || changed)
                            {
                                flog::info("[UDP] {0} ->{1}:{2}",
                                           telemetry,
                                           ::inet_ntoa(arm.sin_addr),
                                           ntohs(arm.sin_port));
                                info_udp = 0;
                            }
                            else
                            {
                                info_udp++;
                            }

                            // обновляем предыдущее состояние
                            prevInit = true;
                            prevFrequency = frequency;
                            prevBandwidth = bandwidth;
                            prevDemodId = demod_id;
                            prevIsPlaying = isPlaying;
                            prevWorkStatus = ws_sent;
                            prevTaskId = task_sent;
                            lastSend = now2;
                        }
                    }
                }

                next_tick = now + tick;
            }

            // 2) короткий сон, чтобы не крутить CPU (с учётом ближайшего тика)
            auto remain = next_tick - std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remain);
            if (ms.count() > 0)
            {
                std::this_thread::sleep_for(
                    (ms < std::chrono::milliseconds(100)) ? ms
                                                          : std::chrono::milliseconds(100));
            }
            else
            {
                // если мы уже "опаздываем" по тикам — сделаем небольшой sleep
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } // while (!stopUdpSender)

        ::close(sockfd);
        flog::info("udpReceiverWorker (telemetry) STOP");
    }

    // ИЗМЕНЕНО: ServerHandler теперь очень быстрый (Производитель)
    void RemoteRadio::ServerHandler(dsp::complex_t *data, int count, void *ctx)
    {
        RemoteRadio *_this = (RemoteRadio *)ctx;
        // flog::info(">>>> ServerHandler GOT DATA! Count: {0}", count);
        std::vector<dsp::complex_t> data_copy(data, data + count);
        _this->audioQueue.push(std::move(data_copy));
    }
};