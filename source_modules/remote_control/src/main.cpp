#include "tcp_client_arm.h"
// #include "connection_manager.h"
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <signal_path/sink.h>
#include <gui/menus/source.h>
#include <version.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())
SDRPP_MOD_INFO{
    /* Name:            */ "remote_control_source",
    /* Description:     */ "Network remote Control Soutce module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 1, 2, 0,
    /* Max instances    */ 2};

ConfigManager config;

class RemoteControlModule : public ModuleManager::Instance
{
public:
    RemoteControlModule(std::string name)
    {
        // _streamName = name;
        // changed = true;
        // Load config
        bool update = false;
        update_menu = false;
        config.acquire();
        if (config.conf.contains("hostname1"))
        {
            std::string hostStr = config.conf["hostname1"];
            strcpy(ip1, hostStr.c_str());
            port[0] = config.conf["port1"];
        }
        else
        {
            config.conf["hostname1"] = "localhost";
            config.conf["port1"] = 7310;
            config.conf["listening1"] = false;
            update = true;
        }
        if (config.conf.contains("hostname2"))
        {
            std::string hostStr = config.conf["hostname2"];
            strcpy(ip2, hostStr.c_str());
            port[1] = config.conf["port2"];
        }
        else
        {
            config.conf["hostname2"] = "localhost";
            config.conf["port2"] = 7320;
            config.conf["listening2"] = false;
            update = true;
        }
        if (config.conf.contains("hostname3"))
        {
            std::string hostStr = config.conf["hostname3"];
            strcpy(ip3, hostStr.c_str());
            port[2] = config.conf["port3"];
        }
        else
        {
            config.conf["hostname3"] = "localhost";
            config.conf["port3"] = 7330;
            config.conf["listening3"] = false;
            update = true;
        }
        if (config.conf.contains("hostname4"))
        {
            std::string hostStr = config.conf["hostname4"];
            strcpy(ip4, hostStr.c_str());
            port[3] = config.conf["port4"];
        }
        else
        {
            config.conf["hostname4"] = "localhost";
            config.conf["port4"] = 7340;
            config.conf["listening4"] = false;
            update = true;
        }
        if (config.conf.contains("hostname5"))
        {
            std::string hostStr = config.conf["hostname5"];
            strcpy(ip5, hostStr.c_str());
            port[4] = config.conf["port5"];
        }
        else
        {
            config.conf["hostname5"] = "localhost";
            config.conf["port5"] = 7350;
            config.conf["listening5"] = false;
            update = true;
        }
        if (config.conf.contains("hostname6"))
        {
            std::string hostStr = config.conf["hostname6"];
            strcpy(ip6, hostStr.c_str());
            port[5] = config.conf["port6"];
        }
        else
        {
            config.conf["hostname6"] = "localhost";
            config.conf["port6"] = 7360;
            config.conf["listening6"] = false;
            update = true;
        }
        if (config.conf.contains("hostname7"))
        {
            std::string hostStr = config.conf["hostname7"];
            strcpy(ip7, hostStr.c_str());
            port[6] = config.conf["port7"];
        }
        else
        {
            config.conf["hostname7"] = "localhost";
            config.conf["port7"] = 7370;
            config.conf["listening7"] = false;
            update = true;
        }
        if (config.conf.contains("hostname8"))
        {
            std::string hostStr = config.conf["hostname8"];
            strcpy(ip8, hostStr.c_str());
            port[7] = config.conf["port8"];
        }
        else
        {
            config.conf["hostname8"] = "localhost";
            config.conf["port8"] = 7380;
            config.conf["listening8"] = false;
            update = true;
        }

        // std::string host = config.conf["hostname1"];
        // strcpy(hostname1, host.c_str());
        // port1 = config.conf["port1"];
        bool startNow = false;
        if (update)
            config.release(true);
        else
            config.release();

        core::configManager.acquire();
        try
        {
            SERVERS_Count = core::configManager.conf["SERVERS_COUNT"];
        }
        catch (const std::exception &e)
        {
            SERVERS_Count = 8;
        }
        try
        {
            Admin = core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            Admin = false;
        }
        core::configManager.release();

        if (SERVERS_Count > MAX_SERVERS)
            SERVERS_Count = MAX_SERVERS;

        lastDataConnectionTimes.resize(SERVERS_Count);

        // Register source
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        // gui::menu.registerEntry(name, menuHandler, this);
        flog::warn(" RegisterInterface: {0}", name);
        sigpath::sourceManager.registerSource("APM", &handler);
        for (int i = 0; i < 8; i++)
            stats[i] = 0;

        clientData = NULL;
        threadEnabled.store(true);

        // core::modComManager.registerInterface("remote_control", name, moduleInterfaceHandler, this);
    }

    ~RemoteControlModule()
    {
        threadEnabled = false;
        // if (currSource == SOURCE_ARM)
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        stop(this);
        // core::modComManager.unregisterInterface("remote_control");
        sigpath::sourceManager.unregisterSource("APM");
    }

    void postInit()
    {
        currSource = sourcemenu::getCurrSource();
        if (currSource == SOURCE_ARM)
            workerThread = std::thread(&RemoteControlModule::worker, this);
    }

    void enable()
    {
        // enabled = true;
        threadEnabled.store(true);
    }

    void disable()
    {
        // enabled = false;
        threadEnabled.store(false);
    }

    bool isEnabled()
    {
        return threadEnabled.load();
    }

private:
    // В remote_control_source (main.cpp модуля)
    static void worker(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        flog::info("Remote Control worker thread started.");

        while (_this->threadEnabled.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!_this->threadEnabled.load())
                break;

            // Поддерживаем жизнь info-каналов
            for (int srv = 0; srv < _this->SERVERS_Count; srv++)
            {
                if (!_this->connectedInfo(srv))
                {
                    startInfo(_this, srv);
                }
            }
            if (!_this->threadEnabled.load())
                break;

            // Проверяем, нужно ли переключаться
            if (!gui::mainWindow.isARMPlaying())
            { // Если кнопка Play не нажата, ничего не делаем
                continue;
            }
            uint8_t current_server_id = gui::mainWindow.getCurrServer();

            // Считаем, сколько секунд прошло с момента последнего подключения
            auto time_since_connect = std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::steady_clock::now() - _this->lastDataConnectionTimes[current_server_id])
                                          .count();

            // Если прошло меньше 3 секунд, пропускаем проверку. Даем соединению стабилизироваться.
            if (time_since_connect < 3)
            {
                continue;
            }

            // Условие сбоя: потерян INFO или DATA канал активного сервера
            bool should_be_playing = gui::mainWindow.isARMPlaying();
            // bool connection_lost = !_this->connectedInfo(current_server_id) || (!gui::mainWindow.isServerIsNotPlaying(current_server_id) && !_this->connected(current_server_id));
            bool connection_lost = !_this->connectedInfo(current_server_id);

            if (should_be_playing && connection_lost)
            {
                flog::error("WORKER: Active server {0} connection lost! Searching for a new server...", current_server_id);

                int next_server_id = -1;
                for (int i = 1; i < _this->SERVERS_Count; i++)
                {
                    int server_to_check = (current_server_id + i) % _this->SERVERS_Count;
                    if (_this->connectedInfo(server_to_check))
                    {
                        next_server_id = server_to_check;
                        break;
                    }
                }

                if (next_server_id != -1)
                {
                    flog::warn("WORKER: Found new server {0}. Requesting player restart.", next_server_id);
                    gui::mainWindow.setCurrServer(next_server_id);
                    gui::mainWindow.pleaseRestartPlayer = true; // Просто взводим флаг
                    // Устанавливаем флаг, который MainWindow::draw увидит и вызовет перезапуск плеера
                    gui::mainWindow.setUpdateMenuRcv0Main(next_server_id, true);
                }
                else
                {
                    flog::error("WORKER: No other active servers found. Stopping player.");
                    if (gui::mainWindow.isPlaying() || gui::mainWindow.isPlaying())
                    {
                        gui::mainWindow.setARMPlayState(false);
                        gui::mainWindow.setPlayState(false);
                    }
                    // gui::mainWindow.setARMPlayState(false);
                }
            }
        }
        flog::info("Remote Control worker thread stopped.");
    }

    static void worker2(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        flog::info("Remote Control worker thread started.");

        while (_this->threadEnabled.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!_this->threadEnabled.load())
                break;

            // 1. Поддерживаем жизнь info-каналов
            for (int srv = 0; srv < _this->SERVERS_Count; srv++)
            {
                if (!_this->connectedInfo(srv))
                {
                    startInfo(_this, srv);
                }
            }
            if (!_this->threadEnabled.load())
                break;

            // 2. Проверяем, не отвалился ли АКТИВНЫЙ сервер, с которого идет поток
            uint8_t current_server_id = gui::mainWindow.getCurrServer();
            // Условие срабатывания: плеер должен быть запущен (_this->running) ИЛИ мы хотим, чтобы он был запущен (arm_playing)
            // И при этом потерян info-канал с текущим сервером.
            bool should_be_playing = gui::mainWindow.isARMPlaying();
            bool connection_lost = !_this->connectedInfo(current_server_id);

            if (should_be_playing && connection_lost)
            {
                flog::error("WORKER: Active server {0} connection lost, but playback is requested!", current_server_id);

                // Ищем следующий доступный сервер по кругу
                int next_server_id = -1;
                for (int i = 1; i < _this->SERVERS_Count; i++)
                {
                    int server_to_check = (current_server_id + i) % _this->SERVERS_Count;
                    if (_this->connectedInfo(server_to_check))
                    {
                        next_server_id = server_to_check;
                        break;
                    }
                }

                if (next_server_id != -1)
                {
                    flog::warn("WORKER: Found new active server {0}. Initiating switch...", next_server_id);
                    // Просто меняем текущий сервер. Логика в MainWindow::draw сделает все остальное.
                    gui::mainWindow.setCurrServer(next_server_id);
                    // Устанавливаем флаг, который MainWindow::draw увидит и вызовет перезапуск плеера
                    gui::mainWindow.setUpdateMenuRcv0Main(next_server_id, true);
                }
                else
                {
                    flog::error("WORKER: No other active servers found. Stopping player.");
                    // Если других серверов нет, останавливаем плеер
                    if (gui::mainWindow.isARMPlaying() || gui::mainWindow.isPlaying())
                    {
                        gui::mainWindow.setARMPlayState(false);
                        gui::mainWindow.setPlayState(false);
                    }
                }
            }
        }
        flog::info("Remote Control worker thread stopped.");
    }
    static void menuSelected(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        flog::info("RemoteControlModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        flog::info("RemoteControlModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        if (_this->running)
        {
            return;
        }
        uint8_t numServer = gui::mainWindow.getCurrServer();

        if (!_this->connectedInfo(numServer))
        {
            flog::error("CANNOT START: Info-channel for server {0} is not connected.", numServer);
            if (gui::mainWindow.isPlaying())
            {
                gui::mainWindow.setPlayState(false);
            }
            return;
        }
        if (gui::mainWindow.isServerIsNotPlaying(numServer))
        {
            _this->running = true;
            flog::warn("gui::mainWindow.isServerIsNotPlaying({0}) {1}", numServer, gui::mainWindow.isServerIsNotPlaying(numServer));
            return;
        }

        double serverSampleRate = gui::mainWindow.getServerSampleRate(numServer);
        if (serverSampleRate > 0)
        {
            flog::info("CORRECTING SAMPLE RATE to {0} Hz for server {1}", serverSampleRate, numServer);
            sigpath::iqFrontEnd.setSampleRate(serverSampleRate);
        }
        else
        {
            flog::warn("Server {0} sample rate is 0. Using default. Waterfall may be incorrect.", numServer);
            sigpath::iqFrontEnd.setSampleRate(10000000.0);
        }

        if (!_this->clientData)
        {
            try
            {
                std::string ip;
                int data_port;
                switch (numServer)
                {
                case 0:
                    ip = _this->ip1;
                    data_port = _this->port[0];
                    break;
                case 1:
                    ip = _this->ip2;
                    data_port = _this->port[1];
                    break;
                case 2:
                    ip = _this->ip3;
                    data_port = _this->port[2];
                    break;
                case 3:
                    ip = _this->ip4;
                    data_port = _this->port[3];
                    break;
                case 4:
                    ip = _this->ip5;
                    data_port = _this->port[4];
                    break;
                case 5:
                    ip = _this->ip6;
                    data_port = _this->port[5];
                    break;
                case 6:
                    ip = _this->ip7;
                    data_port = _this->port[6];
                    break;
                case 7:
                    ip = _this->ip8;
                    data_port = _this->port[7];
                    break;
                default:
                    ip = _this->ip1;
                    data_port = _this->port[0];
                    break;
                }

                flog::info("Connecting DATA channel to {0}:{1}", ip, data_port);
                _this->clientData = server::connectARMData(&_this->stream, ip, data_port, numServer);

                if (_this->clientData && _this->clientData->isOpen())
                {
                    _this->running = true;
                    _this->lastDataConnectionTimes[numServer] = std::chrono::steady_clock::now();
                    flog::info("DATA channel connected successfully.");
                    gui::mainWindow.setServerPlayState(numServer, true);
                }
                else
                {
                    throw std::runtime_error("connectARMData returned a closed or null connection");
                }
            }
            catch (const std::exception &e)
            {
                flog::error("Exception on DATA connect to server {0}: {1}", numServer, e.what());
                // _this->running = false;
                gui::mainWindow.setServerIsNotPlaying(numServer, true);

                if (_this->clientData)
                    _this->clientData = nullptr;
                if (gui::mainWindow.isPlaying())
                {
                    gui::mainWindow.setPlayState(false);
                }
            }
        }
    }

    static void stop(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        // if (!_this->running) { return; }
        _this->running = false;
        if (gui::mainWindow.isARMPlaying())
        {
            gui::mainWindow.setARMPlayState(false);
            gui::mainWindow.setPlayState(false);
        }
        uint8_t numServer = gui::mainWindow.getCurrServer();
        flog::info("stop() numServer '{0}': Stop!", numServer);
        if (_this->clientData)
        {
            _this->clientData->close();
        }
        _this->clientData = NULL;
        // gui::mainWindow.setServerStatus(numServer, ARM_STATUS_STAT_CONTROL);
        flog::info("RemoteControlModule '{0}': Stop!", _this->name);
    }

    static void startInfo(void *ctx, uint8_t numServer)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;

        // Эта проверка больше не нужна, т.к. `worker` вызывает нас только когда соединения нет.
        // if (_this->connectedInfo(numServer)) { return; }

        // Получаем IP и порт для сервера
        std::string _ip;
        // switch-case для получения IP ...
        // (лучше заменить на std::vector<std::string> в будущем)
        switch (numServer)
        {
        case 0:
            _ip = _this->ip1;
            break;
        case 1:
            _ip = _this->ip2;
            break;
        case 2:
            _ip = _this->ip3;
            break;
        case 3:
            _ip = _this->ip4;
            break;
        case 4:
            _ip = _this->ip5;
            break;
        case 5:
            _ip = _this->ip6;
            break;
        case 6:
            _ip = _this->ip7;
            break;
        case 7:
            _ip = _this->ip8;
            break;
        }

        try
        {
            // flog::info("Attempting to create info-connection for server {0}...", numServer);
            std::shared_ptr<server::TCPRemoveARM> clientInfo = server::connectARMInfo(&_this->stream, _ip, _this->port[numServer], numServer);

            // Если соединение успешно, `clientInfo->isOpen()` будет true.
            if (clientInfo && clientInfo->isOpen())
            {
                _this->socketsInfo[numServer] = clientInfo;
                // Синхронизация настроек после успешного подключения
                gui::mainWindow.setServerStatus(numServer, ARM_STATUS_STAT_CONTROL);
                _this->runningCtrl[numServer] = ARM_STATUS_STAT_CONTROL;
                gui::mainWindow.setUpdateMenuSnd0Main(numServer, false);
                gui::mainWindow.setUpdateMenuSnd(false);
                gui::mainWindow.setUpdateMenuSnd2Radio(false);
                gui::mainWindow.setUpdateMenuSnd3Record(false);
                gui::mainWindow.setUpdateLists_srch(true);
                gui::mainWindow.setUpdateMenuSnd5Srch(numServer, true);
                gui::mainWindow.setUpdateLists_scan(true);

                gui::mainWindow.setUpdateMenuSnd6Scan(numServer, true);
                gui::mainWindow.setUpdateMenuSnd7Ctrl(numServer, true);
                flog::info("SUCCESS: Info-connection established for server {0}.", numServer);
            }
            else
            {
                flog::error("FAILURE: connectARMInfo call did not result in an open socket for server {0}.", numServer);
            }
        }
        catch (const std::exception &e)
        {
            if (_this->stats[numServer] >= 5)
            {
                flog::warn("Exception while trying to connect to info-server {0}: {1}", numServer, e.what());
                _this->stats[numServer] = 0;
            }
            _this->stats[numServer]++;
        }
    }

    static void startInfo_old(void *ctx, uint8_t numServer)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        // if (_this->runningCtrl[numServer]) { return; }
        int runningCtrl = gui::mainWindow.getServerStatus(numServer);
        if (runningCtrl > 0)
        {
            return;
        }
        std::shared_ptr<server::TCPRemoveARM> clientInfo = NULL;
        auto _client = _this->socketsInfo.find(numServer);
        if (_client != _this->socketsInfo.end())
        {
            clientInfo = _client->second;
        }
        if (!clientInfo)
        {
            // Connect to the serverInfo
            std::string _ip = _this->ip1;
            switch (numServer)
            {
            case 0:
                _ip = _this->ip1;
                break;
            case 1:
                _ip = _this->ip2;
                break;
            case 2:
                _ip = _this->ip3;
                break;
            case 3:
                _ip = _this->ip4;
                break;
            case 4:
                _ip = _this->ip5;
                break;
            case 5:
                _ip = _this->ip6;
                break;
            case 6:
                _ip = _this->ip7;
                break;
            case 7:
                _ip = _this->ip8;
                break;
            }
            try
            {
                clientInfo = server::connectARMInfo(&_this->stream, _ip, _this->port[numServer], numServer);
                _this->socketsInfo[numServer] = clientInfo;
                // Sync settings
                // clientInfo->setFrequency(_this->freq);
                gui::mainWindow.setServerStatus(numServer, ARM_STATUS_STAT_CONTROL);
                _this->runningCtrl[numServer] = ARM_STATUS_STAT_CONTROL;
                gui::mainWindow.setUpdateMenuSnd0Main(numServer, false);
                gui::mainWindow.setUpdateMenuSnd(false);
                gui::mainWindow.setUpdateMenuSnd2Radio(false);
                gui::mainWindow.setUpdateMenuSnd3Record(false);
                gui::mainWindow.setUpdateLists_srch(true);
                gui::mainWindow.setUpdateMenuSnd5Srch(numServer, true);
                gui::mainWindow.setUpdateLists_scan(true);

                gui::mainWindow.setUpdateMenuSnd6Scan(numServer, true);
                gui::mainWindow.setUpdateMenuSnd7Ctrl(numServer, true);
                flog::info("CONNECTED _this->socketsInfo[{0}], _this->runningCtrl[numServer] {1} ", numServer, _this->runningCtrl[numServer]);
            }
            catch (const std::exception &e)
            {
                // flog::warn("Could connect to TCP server INFO {0}:{1}: {}, {}", _ip, _this->port[numServer], e.what());
                // gui::mainWindow.setPlayState(false);
                return;
            }
        }

        bool _connectedInfo = _this->connectedInfo(numServer); // _this->connected() &&
        // bool status = gui::mainWindow.getUpdateServerStatus(numServer);

        flog::info("connected ==== connectedInfo {0}, _this->runningCtrl[{1}] {2}, status !!", _connectedInfo, numServer, _this->runningCtrl[numServer]);
        if (_connectedInfo)
        {
            // usleep(1000);
            if (clientInfo->getUpdate())
            {
                double _freq = clientInfo->getCurrFrequency();
                _this->volLevel = clientInfo->getVolLevel();

                flog::info("connected 1 _this->freq {0} != _freq = '{1}'!", _this->freq, _freq);
                // _this->freq = _freq;
            }
            else
            {
                // gui::mainWindow.setServerStatus(numServer, ARM_STATUS_STAT_CONTROL);
            }
        }
        flog::info("RemoteControlModule '{0}': Start!", _this->name);
    }

    static void tune(double _freq, void *ctx)
    {
        // if (gui::mainWindow.getUpdateMenuSnd0Main(gui::mainWindow.getCurrServer())) return;
        /// flog::info("___tune.... RemoteControlModule _freq {0}", _freq);
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        _this->freq = _freq;
        // if (_this->running) {
        uint8_t _server = gui::mainWindow.getCurrServer();
        std::shared_ptr<server::TCPRemoveARM> _clientInfo = NULL;
        auto _client = _this->socketsInfo.find(_server);
        if (_client != _this->socketsInfo.end())
        {
            _clientInfo = _client->second;
            _clientInfo->setFrequency(_freq);
        }
    }

    static void menuHandler(void *ctx)
    {
        RemoteControlModule *_this = (RemoteControlModule *)ctx;
        // bool _connected = _this->connected(0);         // _this->connected() &&
        // flog::info("menuHandler..");
        for (uint8_t SRV = 0; SRV < _this->SERVERS_Count; SRV++)
        {                                                    // MAX_SERVERS
            bool _connectedInfo = _this->connectedInfo(SRV); // _this->connected() &&
            std::string ip = "";
            switch (SRV)
            {
            case 0:
                ip = std::string(_this->ip1);
                break;
            case 1:
                ip = std::string(_this->ip2);
                break;
            case 2:
                ip = std::string(_this->ip3);
                break;
            case 3:
                ip = std::string(_this->ip4);
                break;
            case 4:
                ip = std::string(_this->ip5);
                break;
            case 5:
                ip = std::string(_this->ip6);
                break;
            case 6:
                ip = std::string(_this->ip7);
                break;
            case 7:
                ip = std::string(_this->ip8);
                break;
            default:
                ip = "localhost";
                break;
            }
            // gui::mainWindow.getServersName(SRV).c_str()
            if (_connectedInfo)
            {
                // flog::info("version .{0}. vs .{0}.", std::string(VERSION_STR), gui::mainWindow.getVersion(SRV));
                if (std::string(VERSION_STR) == gui::mainWindow.getVersion(SRV))
                {
                    ImGui::TextColored(ImVec4(0.1f, 0.5f, 0.5f, 1.0f), "           %s (v%s, %s:%d)", gui::mainWindow.getServersName(SRV).c_str(), gui::mainWindow.getVersion(SRV).c_str(), ip.c_str(), _this->port[SRV]);
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "           %s (v%s, %s:%d)", gui::mainWindow.getServersName(SRV).c_str(), gui::mainWindow.getVersion(SRV).c_str(), ip.c_str(), _this->port[SRV]);
                }
            }
            else
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "           П%d (%s:%d)", (SRV + 1), ip.c_str(), _this->port[SRV]);
        }
        //====================================================================================
        uint8_t _server = gui::mainWindow.getCurrServer();
        bool _connectedInfo = _this->connectedInfo(_server);
        int status = gui::mainWindow.getServerStatus(_server);
        // flog::info("TRACE... SRV, _connectedInfo {1}, status {2}, _server {3}", SRV, _connectedInfo, status, _server);
        // && status==ARM_STATUS_FULL_CONTROL
        if (_connectedInfo)
        {
            std::shared_ptr<server::TCPRemoveARM> _clientInfo = NULL;
            auto _client = _this->socketsInfo.find(_server);
            if (_client != _this->socketsInfo.end())
            {
                _clientInfo = _client->second;
            }
            _this->volLevel = _clientInfo->getVolLevel();
            // linearGain = getLinearGain();
            _this->update_menu = gui::mainWindow.getUpdateMenuRcv();
            if (_this->update_menu)
            {
                _this->lnaGain = gui::mainWindow.getlnaGain();
                _this->vgaGain = gui::mainWindow.getvgaGain();
                _this->mixerGain = gui::mainWindow.getmixerGain();
                _this->linearGain = gui::mainWindow.getlinearGain();
                _this->sensitiveGain = gui::mainWindow.getsensitiveGain();
                _this->gainMode = gui::mainWindow.getgainMode();
                _this->lnaAgc = gui::mainWindow.getlnaAgc();
                _this->mixerAgc = gui::mainWindow.getmixerAgc();
                _this->select = gui::mainWindow.getselect();
                _this->_updateLinearGain = gui::mainWindow.get_updateLinearGain();
                gui::mainWindow.setUpdateMenuRcv(false);
                // flog::info("TRACE 2 RCV MAIN 361  _this->gainMode {0}, gui::mainWindow.getlnaGain() {1}, _this->linearGain {2}, _this->volLevel {3} !!!", _this->gainMode, gui::mainWindow.getlnaGain(), _this->linearGain, _this->volLevel);
            }
            _this->update_menu = false;

            if (true)
            {
                SmGui::FillWidth();

                // SmGui::ForceSync();
                // if (SmGui::Combo(CONCAT("##_airspy_dev_sel_remove", _this->name), &_this->devId, _this->devListTxt.c_str())) {
                //     flog::info("TRACE devID {0}:{1} menuHandler", _this->devId, _this->devList[_this->devId]);
                //     _this->update_menu = true;
                // }
                if (!_this->Admin)
                {
                    if (_this->gainMode != 1)
                    {
                        _this->gainMode = 1;
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
                        _this->update_menu = true;
                    }
                    SmGui::NextColumn();
                    SmGui::ForceSync();
                    if (SmGui::RadioButton(CONCAT("Довільн.##_airspy_gm_", _this->name), _this->gainMode == 2))
                    {
                        _this->gainMode = 2;
                        _this->update_menu = true;
                    }
                    SmGui::Columns(1, CONCAT("EndAirspyGainModeColumns##_", _this->name), false);
                    SmGui::EndGroup();
                }
                if (_this->gainMode == 0)
                {
                    SmGui::LeftLabel("Підс.");
                    SmGui::FillWidth();
                    if (SmGui::SliderInt(CONCAT("##_airspy_sens_gain_", _this->name), &_this->sensitiveGain, 0, 21))
                    {
                        // gui::mainWindow.setLinearGain(0);
                        _this->update_menu = true;
                    }
                }
                else if (_this->gainMode == 1)
                {
                    SmGui::LeftLabel("Підс.");
                    SmGui::FillWidth();
                    if (SmGui::SliderInt(CONCAT("##_airspy_lin_gain_", _this->name), &_this->linearGain, 0, 21))
                    {
                        _this->update_menu = true;
                    }
                }
                else if (_this->gainMode == 2)
                {
                    // TODO: Switch to a table for alignment
                    // if (_this->lnaAgc) { SmGui::BeginDisabled(); }
                    SmGui::LeftLabel("Підс. МШП");
                    SmGui::FillWidth();
                    if (SmGui::SliderInt(CONCAT("##_airspy_lna_gain_", _this->name), &_this->lnaGain, 0, 15))
                    {
                        _this->update_menu = true;
                    }
                    // if (_this->lnaAgc) { SmGui::EndDisabled(); }

                    // if (_this->mixerAgc) { SmGui::BeginDisabled(); }
                    SmGui::LeftLabel("Підс. Зміш.");
                    SmGui::FillWidth();
                    if (SmGui::SliderInt(CONCAT("##_airspy_mix_gain_", _this->name), &_this->mixerGain, 0, 15))
                    {
                        _this->update_menu = true;
                    }
                    // if (_this->mixerAgc) { SmGui::EndDisabled(); }

                    SmGui::LeftLabel("Підс. VGA");
                    SmGui::FillWidth();
                    if (SmGui::SliderInt(CONCAT("##_airspy_vga_gain_", _this->name), &_this->vgaGain, 0, 15))
                    {
                        _this->update_menu = true;
                    }

                    // AGC Control
                    SmGui::ForceSync();
                    if (SmGui::Checkbox(CONCAT("АРП МШП##_airspy_", _this->name), &_this->lnaAgc))
                    {
                        _this->update_menu = true;
                    }
                    SmGui::ForceSync();
                    if (SmGui::Checkbox(CONCAT("АРП Зміш.##_airspy_", _this->name), &_this->mixerAgc))
                    {
                        _this->update_menu = true;
                    }
                }
                if (_clientInfo && status == ARM_STATUS_FULL_CONTROL)
                {
                    _clientInfo->setUpdate(false);
                    // flog::info("TRACE 3 MAIN");
                    if (_this->update_menu)
                    {
                        double _freq = _clientInfo->getCurrFrequency();
                        flog::info("TRACE menuHandler SEND _this->update_menu {0}, gui::mainWindow.getlinearGain() {1};", _this->update_menu, gui::mainWindow.getlinearGain());
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
                        flog::info("TRACE menuHandler SEND {0}", gui::mainWindow.getUpdateMenuSnd());
                        _this->freq = _freq;
                    }
                }
            }

            // ============================================
            /*
            if (status == ARM_STATUS_FULL_CONTROL)
            {
                ImGui::LeftLabel("Звук");
                float menuWidth = ImGui::GetContentRegionAvail().x;
                // ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                SmGui::FillWidth();
                if (ImGui::SliderFloat("##_clnt_sqelch_lvl_", &_this->volLevel, 0.0f, 1.0f, "%0.2f"))
                {
                    _clientInfo->setVolLevel(_this->volLevel, true);
                    gui::mainWindow.setUpdateMenuSnd0Main(_clientInfo->getCurrSrv(), true);
                }
            }
            */
        }
    }

    bool connected(uint8_t srv)
    {
        // return clientData && clientData->isOpen();
        if (clientData && clientData->isOpen() && clientData->getCurrSrv() == srv)
        {
            return true;
        }
        return false;
    }

    bool connectedInfo(uint8_t srv)
    {
        auto it = socketsInfo.find(srv);
        // Если записи для сервера вообще нет в map, то не подключены.
        if (it == socketsInfo.end())
        {
            return false;
        }

        std::shared_ptr<server::TCPRemoveARM> client = it->second;

        // Если сокет открыт, все хорошо.
        if (client && client->isOpen())
        {
            return true;
        }

        // Если мы здесь, значит сокет мертв. Нужно выполнить очистку.
        flog::warn("Detected dead info-socket for server {0}. Cleaning up.", srv);

        // `close()` вызовет `join()` для потока и освободит ресурсы.
        if (client)
        {
            client->close();
        }

        // Удаляем невалидную запись из map.
        socketsInfo.erase(it);

        // Уведомляем GUI, что соединение разорвано.
        gui::mainWindow.setServerStatus(srv, ARM_STATUS_NOT_CONTROL);
        gui::mainWindow.fullConnection[srv] = false; // Если есть такой флаг

        return false;
    }

    int SERVERS_Count = 8;
    std::string name;
    // bool enabled = true;
    std::atomic<bool> threadEnabled;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
    std::shared_ptr<server::TCPRemoveARM> clientData;

    std::map<uint8_t, std::shared_ptr<server::TCPRemoveARM>> socketsInfo;

    // std::vector<std::unique_ptr<ConnectionManager>> connectionManagers;
    // std::shared_ptr<server::TCPRemoveARM> client1;
    // std::shared_ptr<server::TCPRemoveARM> currClientInfo;

    bool running = false;
    int runningCtrl[8] = {ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL,
                          ARM_STATUS_NOT_CONTROL};

    double freq = 123000;

    char ip1[1024] = "localhost";
    char ip2[1024] = "localhost";
    char ip3[1024] = "localhost";
    char ip4[1024] = "localhost";
    char ip5[1024] = "localhost";
    char ip6[1024] = "localhost";
    char ip7[1024] = "localhost";
    char ip8[1024] = "localhost";

    int port[8] = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};

    int srId = 0;
    int directSamplingId = 0;
    int ppm = 0;
    int gain = 0;
    bool biasTee = false;
    bool offsetTuning = false;
    bool rtlAGC = false;
    bool tunerAGC = false;

    float datarate = 0;
    float frametimeCounter = 0;
    float volLevel = 0.5;
    int gainMode = 1;

    int lnaGain = 0;
    int vgaGain = 0;
    int mixerGain = 0;
    int linearGain = 0;
    int sensitiveGain = 0;
    bool lnaAgc = false;
    bool mixerAgc = false;
    bool select = false;
    bool _updateLinearGain = false;
    bool update_menu = true;

    int devId = 0;
    std::vector<uint64_t> devList;
    std::string devListTxt;
    int cnt = 0;
    std::string currSource;
    bool Admin;
    int stats[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<std::chrono::steady_clock::time_point> lastDataConnectionTimes;
};

MOD_EXPORT void _INIT_()
{
    json def = json({});
    config.setPath(core::args["root"].s() + "/remote_ctrl.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void *_CREATE_INSTANCE_(std::string name)
{
    return new RemoteControlModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance)
{
    delete (RemoteControlModule *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
}