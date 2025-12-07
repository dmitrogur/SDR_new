#include <server.h>
#include <aster_server.h>
#include "imgui.h"
#include <stdio.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/icons.h>
#include <version.h>
#include <atomic>
#include <queue>
#include <server_protocol.h>
#include <atomic>
#include <map>
#include <cstring>

#include <utils/flog.h>
#include <gui/widgets/bandplan.h>
#include <stb_image.h>
#include <config.h>
#include <core.h>
#include <filesystem>
#include <gui/menus/theme.h>
#include <backend.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <signal_path/signal_path.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

void setproctitle(const char *fmt, ...)
{
    static char title[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(title, sizeof(title), fmt, args);
    va_end(args);
    //    prctl(PR_SET_NAME, title, 0, 0, 0);
    static char dash[100];
    strcpy(dash, "--");
    char *new_argv[] = {core::args.systemArgv[0], dash, title, NULL};
    memcpy(core::args.systemArgv, new_argv, sizeof(new_argv));
}

#else
void setproctitle(const char *fmt, ...)
{
}
#endif

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>

#ifdef _WIN32
#include <Windows.h>
#endif

#ifndef INSTALL_PREFIX
#ifdef __APPLE__
#define INSTALL_PREFIX "/usr/local"
#else
#define INSTALL_PREFIX "/usr"
#endif
#endif

namespace core
{
    bool DEBUG = false;
    ConfigManager configManager;
    ModuleManager moduleManager;
    ModuleComManager modComManager;
    CommandArgsParser args;
    std::atomic<bool> g_isExiting{false};

    void setInputSampleRate(double samplerate)
    {
        // Forward this to the server
        if (args["server"].b())
        {
            server::setInputSampleRate(samplerate);
            return;
        }
        // Update IQ frontend input samplerate and get effective samplerate
        sigpath::iqFrontEnd.setSampleRate(samplerate);
        double effectiveSr = sigpath::iqFrontEnd.getEffectiveSamplerate();
        // Reset zoom
        if (DEBUG)
            flog::error("[SERVER-SETUP/setInputSampleRate RPM/ARM] Forcing VFO effectiveSr={0}", effectiveSr);
        gui::waterfall.setBandwidth(effectiveSr);
        gui::waterfall.setViewOffset(0);

        // core::configManager.acquire();
        std::string _source = core::configManager.conf["source"];
        // core::configManager.release();
        flog::info("setInputSampleRate _source {0}", _source);
        if (_source == "Airspy" || _source == "ARM")
        {
            if (effectiveSr > VIEWBANDWICH)
                effectiveSr = VIEWBANDWICH;
            gui::waterfall.setViewBandwidth(effectiveSr);
            gui::mainWindow.setViewBandwidthSlider(0.922);
        }
        else
        {
            gui::waterfall.setViewBandwidth(effectiveSr);
            gui::mainWindow.setViewBandwidthSlider(1.0);
        }

        // Debug logs
        flog::info("New DSP samplerate: {0} (source samplerate is {1})", effectiveSr, samplerate);
    }

    /// FORK SERVER

    int forkPipe[2];
    int forkResult[2];

    struct ForkServerResults
    {
        int seq = 0;
        int pid = 0;
        bool terminated = false;
        int wstatus = 0;
    };

    std::unordered_map<int, std::shared_ptr<SpawnCommand>> forkInProgress;
    std::mutex forkInProgressLock;

    bool forkIt(const std::shared_ptr<SpawnCommand> &cmd)
    {
#ifndef _WIN32
        static std::atomic_int _seq;
        forkInProgressLock.lock();
        cmd->seq = _seq++;
        cmd->completed = false;
        forkInProgress[cmd->seq] = cmd;
        forkInProgressLock.unlock();
        if (sizeof(*cmd.get()) != write(forkPipe[1], cmd.get(), sizeof(*cmd.get())))
        {
            return false;
        }
#endif
        return true;
    }

    void removeForkInProgress(int seq)
    {
        forkInProgressLock.lock();
        forkInProgress.erase(seq);
        forkInProgressLock.unlock();
    }

    void cldHandler(int i)
    {
#ifndef _WIN32
        //        write(1, "cldHandler\n", strlen("cldHandler\n"));
        //        flog::info("SIGCLD, waiting i={}", i);
        int wstatus;
        auto q = wait(&wstatus);
        //        flog::info("SIGCLD, waited = {}, status={}", q, wstatus);
        ForkServerResults res;
        res.seq = -1;
        res.pid = q;
        res.terminated = true;
        res.wstatus = wstatus;
        //        flog::info("FORKSERVER, sending pid death: {}", q);
        if (write(forkResult[1], &res, sizeof(res)) < 0)
        {
            flog::warn("Failed to write fork result");
        }
#endif
    }

    void startForkServer()
    {
#ifndef _WIN32
        if (pipe(forkPipe))
        {
            flog::error("Cannot create pipe.");
            exit(1);
        }
        if (pipe(forkResult))
        {
            flog::error("Cannot create pipe.");
            exit(1);
        }
        if (fork() == 0)
        {

            setproctitle("sdrpp (sdr++) fork server (spawning decoders)");

#ifdef __linux__
//            int priority = 19; // background, the least priority
//            int which = PRIO_PROCESS; // set priority for the current process
//            pid_t pid = 0; // use the current process ID
//            int ret = setpriority(which, pid, priority);
//            if (ret != 0) {
//                // error handling
//            }
#endif

            //            flog::info("FORKSERVER: fork server runs");
            int myPid = getpid();
            std::thread checkParentAlive([=]()
                                         {
                while (true) {
                    sleep(1);
                    if (getppid() == 1) {
                        kill(myPid, SIGHUP);
                        break;
                    }
                } });
            checkParentAlive.detach();
#ifndef SIGCLD
#define SIGCLD 17
#endif
            signal(SIGCLD, cldHandler);
            bool running = true;
            while (running)
            {
                SpawnCommand cmd;
                if (sizeof(cmd) != read(forkPipe[0], &cmd, sizeof(cmd)))
                {
                    flog::warn("FORKSERVER, misread command");
                    continue;
                }

                auto newPid = fork();
                if (0 == newPid)
                {
                    //                    flog::info("FORKSERVER {}, forked ok", cmd.info);
                    auto &args = cmd;
                    std::string execDir = args.executable;
                    auto pos = execDir.rfind('/');
                    if (pos != std::string::npos)
                    {
                        execDir = execDir.substr(0, pos);
                        execDir = "LD_LIBRARY_PATH=" + execDir;
                        putenv((char *)execDir.c_str());
                        //                        flog::info("FORKSERVER, in child, before exec putenv {}", execDir);
                    }
                    //                    flog::info("decoderPath={}", args.executable);
                    //                    flog::info("FT8 Decoder({}): executing: {}", cmd.info, args.executable);

                    if (true)
                    {
                        close(0);
                        close(1);
                        close(2);
                        open("/dev/null", O_RDONLY, 0600);                     // input
                        open(cmd.outPath, O_CREAT | O_TRUNC | O_WRONLY, 0600); // out
                        open(cmd.errPath, O_CREAT | O_TRUNC | O_WRONLY, 0600); // err
                    }
                    std::vector<char *> argsv;
                    for (int i = 0; i < args.nargs; i++)
                    {
                        argsv.emplace_back(&args.args[i][0]);
                    }
                    argsv.emplace_back(nullptr);
                    auto err = execv((const char *)(&args.executable[0]), argsv.data());
                    static auto q = errno;
                    if (err < 0)
                    {
                        perror("exec");
                    }
                    if (write(1, "\nBefore process exit\n", strlen("\nBefore process exit\n")) < 0)
                    {
                        // Ignore error on process exit
                    }
                    close(0);
                    close(1);
                    close(2);
                    abort(); // exit does not terminate well.

                    flog::warn("FORKSERVER, back from forked ok");
                }
                else
                {
                    ForkServerResults res;
                    res.seq = cmd.seq;
                    res.pid = newPid;
                    //                    flog::info("FORKSERVER ({}), sending pid: {}", cmd.info, newPid);
                    if (write(forkResult[1], &res, sizeof(res)) < 0)
                    {
                        flog::warn("Failed to write fork result");
                    }
                }
            }
        }
        else
        {
            std::thread resultReader([]()
                                     {
                SetThreadName("forkserver_resultread");
//                flog::info("FORKSERVER: resultreader started");
                while (true) {
                    ForkServerResults res;
                    if (0 != read(forkResult[0], &res, sizeof(res))) {
                        forkInProgressLock.lock();
                        auto found = forkInProgress.find(res.seq);
                        if (res.seq < 0) {
                            for (auto it : forkInProgress) {
                                if (it.second->pid == res.pid) {
                                    found = forkInProgress.find(it.first);
                                    break;
                                }
                            }
                        }
                        if (found != forkInProgress.end()) {
                            if (res.pid != 0) {
                                found->second->pid = res.pid;
                            }
                            if (res.terminated != 0) {
//                                flog::info("FORKSERVER: marking terminated: pid={}, res={}", res.pid, (void*)&res);
                                found->second->completeStatus = res.wstatus;
                                found->second->completed = true;
                            }
                        }
                        else {
//                            flog::info("FORKSERVER: not found mark status: pid={} seq={}", res.pid, res.seq);
                        }
                        forkInProgressLock.unlock();
                    }
                } });
            resultReader.detach();
        }
#endif
    }
};

std::string exec(const char *cmd)
{
    char buffer[1280];
    std::string result = "";
    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        throw std::runtime_error("popen() failed!");
    try
    {
        while (fgets(buffer, sizeof buffer, pipe) != NULL)
        {
            result += buffer;
        }
    }
    catch (...)
    {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    // flog::info("result =  {0}", result);
    return result;
}

bool unloadAllInstanceVirtCards(std::string vsink)
{
    std::string vcard = "pactl list short modules | grep '" + vsink + "'";
    std::string result = "";
    do
    {
        result = exec(vcard.c_str());
        // flog::info("unloadVirtCards....{0}, cmd: {1}, result ={2}", bookmarks.size(), vcard, result);
        if (result != "")
        {
            //     exec("sync");
            std::string num = "";
            for (int i = 0; i < 10; i++)
            {
                if (result[i] < 48)
                    break;
                num = num + result[i];
            }
            flog::info("Deleting Vcard with num = {0}", num);
            std::string unload_cmd = "pactl unload-module " + num;
            exec(unload_cmd.c_str());
            // flog::info("unloadVirtCards....{0}, cmd: {1}, result ={2}\n", bookmarks.size(), vcard, result);
        }
    } while (result != "");
    return true;
}

//================================================================================

// main
int sdrpp_main(int argc, char *argv[])
{
    flog::info("ASTER/MALVA-RPM v" VERSION_STR);

    // aster_server::AServer *aServer;

#ifdef IS_MACOS_BUNDLE
    // If this is a MacOS .app, CD to the correct directory
    auto execPath = std::filesystem::absolute(argv[0]);
    chdir(execPath.parent_path().string().c_str());
#endif

    // Define command line options and parse arguments
    core::args.defineAll();
    if (core::args.parse(argc, argv) < 0)
    {
        return -1;
    }

    // Show help and exit if requested
    if (core::args["help"].b())
    {
        core::args.showHelp();
        return 0;
    }

    bool serverMode = (bool)core::args["server"];

    // Check root directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root))
    {
        flog::warn("Root directory {0} does not exist, creating it", root);
        if (!std::filesystem::create_directories(root))
        {
            flog::error("Could not create root directory {0}", root);
            return -1;
        }
    }

    // Check that the path actually is a directory
    if (!std::filesystem::is_directory(root))
    {
        flog::error("{0} is not a directory", root);
        return -1;
    }

    json machConfig;
    machConfig["machine"] = "";
    machConfig["machines"] = json::array();
    machConfig["machines"][0] = "5de76858c62d403da7cc4f6aff0c3abb";

    flog::info("Loading machines");
    core::configManager.setPath(root + "/res/ps.json");
    core::configManager.load(machConfig);
    // core::configManager.enableAutoSave();
    core::configManager.acquire();
    std::vector<std::string> machines = core::configManager.conf["machines"];
    core::configManager.release();

    for (auto elem : machines)
    {
        // flog::info("_____ {0}", elem);
    }
    std::string result = exec("cat /etc/machine-id");
    if (!result.empty())
    {
        result.pop_back();
    }

    if (std::find(machines.begin(), machines.end(), result.c_str()) != machines.end())
    {
        flog::info("OK. Authorization was successful result = {0}", result);
    }
    else
    {
        flog::error("Authorization was not successful((");
        return 1;
    }

    std::string extension = ".pid";
    std::filesystem::directory_iterator iterator(root);
    std::string str_pid = std::to_string(getpid());
    for (; iterator != std::filesystem::end(iterator); iterator++)
    {
        if (iterator->path().extension() == extension)
        {
            str_pid = iterator->path().stem().string();
            std::string _file = iterator->path().string();
            flog::warn("Module was launch {0} str_pid = {1}", _file, str_pid);
            std::string _cmd = "ps -a | grep " + str_pid;
            result = exec(_cmd.c_str());
            if (result != "")
            {
                flog::error("Module is launch {0}, str_pid = {1}. Program will stop!", _file, str_pid);
                return 1;
            }
            else
            {
                std::filesystem::remove(_file);
                flog::warn("Delete {0}", _file);
            }
        }
    }

    flog::info("getpid()= {0}", getpid());
    str_pid = std::to_string(getpid());
    std::string pidfile = root + "//" + str_pid + ".pid";
    std::filesystem::path my_path = {pidfile}; // str_pid;
    std::ofstream _ofs(my_path);
    // _ofs.rdbuf()->open(pidfile, std::ios_base::in);
    _ofs << str_pid;
    _ofs.close();
    /*
    if (!std::filesystem::is_regular_file(pidfile)) {
        flog::error("Module is not running {0}", pidfile);
    } else {
        flog::error("Module is running {0}", pidfile);
    }
    */

    // ======== DEFAULT CONFIG ========
    json defConfig;

    defConfig["Admin"] = false;
    defConfig["InstanceName"] = "P0";
    defConfig["InstanceNum"] = 1;
    defConfig["IsARM"] = false;
    defConfig["IsNotPlaying"] = false;
    defConfig["AsterPlus"] = false;
    defConfig["IsServer"] = true;
    defConfig["MAX_CHANNELS"] = 6;
    defConfig["PathJson"] = "/opt/banks";
    defConfig["PathWav"] = "/opt/recordings";
    defConfig["RadioMode"] = 0;
    defConfig["RecDirectoryShort"] = "/var/lib/avr/cws/data/receiver/va_only";
    defConfig["RecDirectoryLong"] = "/var/lib/avr/cws/data/receiver/records";
    defConfig["SIport"] = 63100;
    defConfig["SERVERS_COUNT"] = 8;
    defConfig["SaveInDirMalva"] = false;
    defConfig["ServerPort"] = 7310;
    defConfig["SignalIndf"] = false;
    defConfig["Url"] = "http://10.10.10.100:8101/event";
    defConfig["USE_curl"] = false;
    defConfig["bandColors"]["amateur"] = "#FF0000FF";
    defConfig["bandColors"]["aviation"] = "#00FF00FF";
    defConfig["bandColors"]["broadcast"] = "#0000FFFF";
    defConfig["bandColors"]["marine"] = "#00FFFFFF";
    defConfig["bandColors"]["military"] = "#FFFF00FF";

    defConfig["bandPlan"] = "General";
    defConfig["bandPlanEnabled"] = false;
    defConfig["bandPlanPos"] = 0;
    defConfig["centerTuning"] = false;
    defConfig["colorMap"] = "Classic";
    defConfig["decimationPower"] = 0;
    defConfig["fastFFT"] = false;
    defConfig["fftHold"] = false;
    defConfig["fftHoldSpeed"] = 2;
    defConfig["fftSmoothing"] = true;
    defConfig["fftSmoothingSpeed"] = 50;
    defConfig["fftHeight"] = 300;
    defConfig["fftRate"] = 286;
    defConfig["fftSize"] = 32768;
    defConfig["fftWindow"] = 1;
    defConfig["flagVA"] = false;
    defConfig["frequency"] = 110000000.0;
    defConfig["fullWaterfallUpdate"] = false;
    defConfig["fullscreen"] = false;

    defConfig["hostname"] = "localhost";
    defConfig["icon"] = "/icons/sdrpp_1.png";
    defConfig["invertIQ"] = false;
    defConfig["iqCorrection"] = false;
#ifdef __ANDROID__
    defConfig["lockMenuOrder"] = true;
#else
    defConfig["lockMenuOrder"] = false;
#endif
    defConfig["max"] = 0.0;
    defConfig["maxCountInScanBank"] = 256;
    defConfig["maxRecDuration"] = 10;
    defConfig["maxRecShortDur_sec"] = 3;
    defConfig["maximized"] = false;
    defConfig["maxRecShortDur_sec"] = 2;
    // Menu
    defConfig["menuElements"] = json::array();

    defConfig["menuElements"][0]["name"] = "Завантаження"; //"Радіоприймач";
    defConfig["menuElements"][0]["open"] = false;

    defConfig["menuElements"][1]["name"] = "Канал приймання";
    defConfig["menuElements"][1]["open"] = false;

    defConfig["menuElements"][2]["name"] = "Запис";
    defConfig["menuElements"][2]["open"] = false;

    defConfig["menuElements"][3]["name"] = "Виводи ЦП";
    defConfig["menuElements"][3]["open"] = false;

    defConfig["menuElements"][4]["name"] = "Налаштування";
    defConfig["menuElements"][4]["open"] = false;

    defConfig["menuWidth"] = 300;
    defConfig["min"] = -120.0;

    for (int serverId = 0; serverId < 8; serverId++)
    {
        std::string keyMax = "fftMax_server_" + std::to_string(serverId);
        std::string keyMin = "fftMin_server_" + std::to_string(serverId);
        defConfig[keyMax] = -100;
        defConfig[keyMin] = 0.0;
    }

    // Module instances
    defConfig["moduleInstances"]["Airspy"]["module"] = "airspy_source";
    defConfig["moduleInstances"]["Airspy"]["enabled"] = true;
    defConfig["moduleInstances"]["Азалія-сервер"]["module"] = "airspy_source";
    defConfig["moduleInstances"]["Азалія-сервер"]["enabled"] = true;
    defConfig["moduleInstances"]["AirspyHF+ Source"]["module"] = "airspyhf_source";
    defConfig["moduleInstances"]["AirspyHF+ Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Audio Source"]["module"] = "audio_source";
    defConfig["moduleInstances"]["Audio Source"]["enabled"] = true;
    // defConfig["moduleInstances"]["РПП Сервер"]["module"] = "spyserver_source";
    // defConfig["moduleInstances"]["РПП Сервер"]["enabled"] = true;

    defConfig["moduleInstances"]["BladeRF Source"]["module"] = "bladerf_source";
    defConfig["moduleInstances"]["BladeRF Source"]["enabled"] = true;
    defConfig["moduleInstances"]["File Source"]["module"] = "file_source";
    defConfig["moduleInstances"]["File Source"]["enabled"] = true;
    /*
    defConfig["moduleInstances"]["HackRF Source"]["module"] = "hackrf_source";
    defConfig["moduleInstances"]["HackRF Source"]["enabled"] = false;
    defConfig["moduleInstances"]["Hermes Source"]["module"] = "hermes_source";
    defConfig["moduleInstances"]["Hermes Source"]["enabled"] = false;
    defConfig["moduleInstances"]["LimeSDR Source"]["module"] = "limesdr_source";
    defConfig["moduleInstances"]["LimeSDR Source"]["enabled"] = false;
    defConfig["moduleInstances"]["PlutoSDR Source"]["module"] = "plutosdr_source";
    defConfig["moduleInstances"]["PlutoSDR Source"]["enabled"] = false;
    //    defConfig["moduleInstances"]["RFspace Source"]["module"] = "rfspace_source";
    //    defConfig["moduleInstances"]["RFspace Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RTL-SDR Source"]["module"] = "rtl_sdr_source";
    defConfig["moduleInstances"]["RTL-SDR Source"]["enabled"] = false;

    defConfig["moduleInstances"]["RTL-TCP"]["module"] = "rtl_tcp_source";
    defConfig["moduleInstances"]["RTL-TCP"]["enabled"] = true;
    defConfig["moduleInstances"]["SDRplay Source"]["module"] = "sdrplay_source";
    defConfig["moduleInstances"]["SDRplay Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SoapySDR Source"]["module"] = "soapy_source";
    defConfig["moduleInstances"]["SoapySDR Source"]["enabled"] = true;


    defConfig["moduleInstances"]["Мальва-сервер Source"]["module"] = "sdrpp_server_source";
    defConfig["moduleInstances"]["Мальва-сервер Source"]["enabled"] = true;
    //    defConfig["moduleInstances"]["Azalea UDP Client Source"]["module"] = "udp_server_source";
    //    defConfig["moduleInstances"]["Azalea UDP Client Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Азалія-клієнт Source"]["module"] = "tcp_client_source";
    defConfig["moduleInstances"]["Азалія-клієнт Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Сервер приймання"]["module"] = "spyserver_source";
    defConfig["moduleInstances"]["Сервер приймання"]["enabled"] = true;
    */

    // defConfig["moduleInstances"]["Аудіо вивід"] = "audio_sink";
    defConfig["moduleInstances"]["Audio"] = "audio_sink";
    defConfig["moduleInstances"]["Вивід в мережу"] = "network_sink";

    defConfig["moduleInstances"]["Канал приймання"] = "radio";

    // defConfig["moduleInstances"]["Сервер РПП"] = "remote_radio";
    // defConfig["moduleInstances"]["Сервер РПП"]["enabled"] = true;

    // defConfig["moduleInstances"]["Frequency Manager"] = "frequency_manager";
    defConfig["moduleInstances"]["Запис"] = "recorder";
    // defConfig["moduleInstances"]["Rigctl Server"] = "rigctl_server";
    //  defConfig["moduleInstances"]["Rigctl Client"] = "rigctl_client";
    //  TODO: Enable rigctl_client when ready
    //  defConfig["moduleInstances"]["Scanner"] = "scanner";
    //  TODO: Enable scanner when ready

    bool DEBUG = false;

    defConfig["modules"] = json::array();
    defConfig["offset"] = 0.0;
    defConfig["offsetMode"] = (int)0; // Off
    defConfig["port"] = 4000;
#if defined(_WIN32)
    defConfig["modulesDirectory"] = "./modules";
    defConfig["resourcesDirectory"] = "./res";
#elif defined(IS_MACOS_BUNDLE)
    defConfig["modulesDirectory"] = "../Plugins";
    defConfig["resourcesDirectory"] = "../Resources";
#elif defined(__ANDROID__)
    defConfig["modulesDirectory"] = root + "/modules";
    defConfig["resourcesDirectory"] = root + "/res";
#else
    defConfig["modulesDirectory"] = "./root_dev/modules"; //  INSTALL_PREFIX "/lib/sdrpp/plugins";
    defConfig["resourcesDirectory"] = "./root_dev/res";   //  INSTALL_PREFIX "/share/sdrpp";
#endif

    defConfig["showFFT"] = true;
    defConfig["showMenu"] = true;
    defConfig["showWaterfall"] = false;
    defConfig["source"] = "Airspy";

    defConfig["streams"]["Канал приймання"]["muted"] = false;
    defConfig["streams"]["Канал приймання"]["sink"] = "Audio";
    defConfig["streams"]["Канал приймання"]["volume"] = 0.5f;
    // Themes
    defConfig["theme"] = "Темна";
#ifdef __ANDROID__
    defConfig["uiScale"] = 3.0f;
#else
    defConfig["uiScale"] = 1.0f;
#endif
    defConfig["vfoColors"]["Канал приймання"] = "#FFFFFF";
    defConfig["vfoOffsets"] = json::object();

    defConfig["windowSize"]["h"] = 720;
    defConfig["windowSize"]["w"] = 1280;

    // Load config
    flog::info("Loading config");
    core::configManager.setPath(root + "/config.json");
    core::configManager.setPathCpy(root + "/_config.json");
    core::configManager.load(defConfig);
    core::configManager.enableAutoSave();
    core::configManager.acquire();

    // Устанавливаем "receivers" как массив
    defConfig["receivers"] = nlohmann::json::array();
    // Добавляем 8 приёмников с дефолтными значениями
    for (int i = 0; i < 8; ++i)
    {
        defConfig["receivers"].push_back({{"mode", 0},
                                          {"bank", 0}});
    }

    // Fix missing elements in config
    for (auto const &item : defConfig.items())
    {
        if (!core::configManager.conf.contains(item.key()))
        {
            flog::info("Missing key in config {0}, repairing", item.key());
            core::configManager.conf[item.key()] = defConfig[item.key()];
        }
    }
    // Remove unused elements
    auto items = core::configManager.conf.items();
    for (auto const &item : items)
    {
        if (!defConfig.contains(item.key()))
        {
            flog::info("Unused key in config {0}, repairing", item.key());
            core::configManager.conf.erase(item.key());
        }
    }
    flog::info("OK ..");

    // Update to new module representation in config if needed
    for (auto [_name, inst] : core::configManager.conf["moduleInstances"].items())
    {
        if (!inst.is_string())
        {
            continue;
        }
        std::string mod = inst;
        json newMod;
        newMod["module"] = mod;
        newMod["enabled"] = true;
        core::configManager.conf["moduleInstances"][_name] = newMod;
    }
    flog::info("OK ...");

    int numInstance = core::configManager.conf["InstanceNum"];
    std::string vsink = "vsink_" + std::to_string(numInstance);
    unloadAllInstanceVirtCards(vsink);

    // Load UI scaling
    style::uiScale = core::configManager.conf["uiScale"];

    core::configManager.release(true);

    if (serverMode)
    {
        return server::main();
    }

    core::configManager.acquire();
    std::string resDir = core::configManager.conf["resourcesDirectory"];
    json bandColors = core::configManager.conf["bandColors"];
    core::configManager.release();

    // Assert that the resource directory is absolute and check existence
    resDir = std::filesystem::absolute(resDir).string();
    if (!std::filesystem::is_directory(resDir))
    {
        flog::error("Resource directory doesn't exist! Please make sure that you've configured it correctly in config.json (check readme for details)");
        return 1;
    }

    // Initialize backend
    int biRes = backend::init(resDir);
    if (biRes < 0)
    {
        return biRes;
    }

    // Initialize SmGui in normal mode
    SmGui::init(false);

    if (!style::loadFonts(resDir))
    {
        return -1;
    }
    thememenu::init(resDir);
    LoadingScreen::init();

    LoadingScreen::show("Loading icons");
    flog::info("Loading icons");
    if (!icons::load(resDir))
    {
        return -1;
    }
    LoadingScreen::show("Loading band plans");
    flog::info("Loading band plans");
    bandplan::loadFromDir(resDir + "/bandplans");

    LoadingScreen::show("Loading band plan colors");
    flog::info("Loading band plans color table");
    bandplan::loadColorTable(bandColors);

    gui::mainWindow.init();

    flog::info("Ready.");

    // aster_server::main();
    // aServer->start();
    // core::workerThread = std::thread(&aster_server::worker(), NULL);
    // Run render loop (TODO: CHECK RETURN VALUE)
    backend::renderLoop();

    core::g_isExiting = true;
    if (DEBUG)
        flog::error("SHUTDOWN INITIATED, g_isExiting IS NOW TRUE");
    // ==========================================================
    // НОВЫЙ БЛОК: КОРРЕКТНОЕ ЗАВЕРШЕНИЕ РАБОТЫ
    // ==========================================================
    flog::info("Shutdown sequence started...");
    if (DEBUG)
        flog::error("!!!!!!!!!! MAIN THREAD: Exited renderLoop. Starting shutdown. !!!!!!!!!!");
    // 1. Останавливаем плеер. Это вызовет stop() для источника.
    if (gui::mainWindow.isPlaying())
    {
        flog::info("...Stopping player.");
        gui::mainWindow.setPlayState(false);
    }

    // 2. Явно останавливаем RemoteRadio.
    //    Это остановит его потоки и отпишется от iqFrontEnd.
    flog::info("...Stopping RemoteRadio.");
    sigpath::remoteRadio.stop();
    if (DEBUG)
        flog::error("!!!!!!!!!! MAIN THREAD: Shutdown sequence complete. About to exit. !!!!!!!!!!");
    // 3. Останавливаем iqFrontEnd.
    flog::info("...Stopping IQFrontEnd.");
    sigpath::iqFrontEnd.stop();

    extension = ".pid";
    iterator = std::filesystem::begin(iterator);
    const std::filesystem::path p = root;
    for (auto &entry : std::filesystem::directory_iterator(p))
    {
        if (entry.path().extension() == extension)
        {
            std::string _file = entry.path().string();
            std::filesystem::remove(_file);
            if (DEBUG)
                flog::warn("Delete {0}", _file);
        }
    }
    flog::info("Exiting successfully");
    return 0;
}
