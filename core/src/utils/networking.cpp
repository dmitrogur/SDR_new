#include <utils/networking.h>
#include <assert.h>
#include <utils/flog.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{

#ifdef _WIN32
    extern bool winsock_init = false;
#endif

    ConnClass::ConnClass(Socket sock, struct sockaddr_in raddr, bool udp)
    {
        _sock = sock;
        _udp = udp;
        remoteAddr = raddr;
        connectionOpen = true;
        readWorkerThread = std::thread(&ConnClass::readWorker, this);
        writeWorkerThread = std::thread(&ConnClass::writeWorker, this);
    }

    ConnClass::~ConnClass()
    {
        ConnClass::close();
    }

    void ConnClass::close()
    {
        std::lock_guard lck(closeMtx);
        // Set stopWorkers to true
        {
            std::lock_guard lck1(readQueueMtx);
            std::lock_guard lck2(writeQueueMtx);
            stopWorkers = true;
        }

        // Notify the workers of the change
        readQueueCnd.notify_all();
        writeQueueCnd.notify_all();

        if (connectionOpen)
        {
#ifdef _WIN32
            closesocket(_sock);
#else
            ::shutdown(_sock, SHUT_RDWR);
            ::close(_sock);
#endif
        }

        // Wait for the theads to terminate
        if (readWorkerThread.joinable())
        {
            readWorkerThread.join();
        }
        if (writeWorkerThread.joinable())
        {
            writeWorkerThread.join();
        }

        {
            std::lock_guard lck(connectionOpenMtx);
            connectionOpen = false;
        }
        connectionOpenCnd.notify_all();
    }

    bool ConnClass::isOpen()
    {
        return connectionOpen;
    }

    void ConnClass::waitForEnd()
    {
        std::unique_lock lck(readQueueMtx);
        connectionOpenCnd.wait(lck, [this]()
                               { return !connectionOpen; });
    }

    int ConnClass::read(int count, uint8_t *buf, bool enforceSize)
    {
        if (!connectionOpen)
        {
            return -1;
        }
        std::lock_guard lck(readMtx);
        int ret;

        if (_udp)
        {
            socklen_t fromLen = sizeof(remoteAddr);
            ret = recvfrom(_sock, (char *)buf, count, 0, (struct sockaddr *)&remoteAddr, &fromLen);
            if (ret <= 0)
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return -1;
            }
            return count;
        }

        int beenRead = 0;
        while (beenRead < count)
        {
            ret = recv(_sock, (char *)&buf[beenRead], count - beenRead, 0);

            if (ret <= 0)
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return -1;
            }

            if (!enforceSize)
            {
                return ret;
            }

            beenRead += ret;
        }

        return beenRead;
    }

    bool ConnClass::write(int count, uint8_t *buf)
    {
        if (!connectionOpen)
        {
            return false;
        }
        std::lock_guard lck(writeMtx);
        int ret;

        if (_udp)
        {
            ret = sendto(_sock, (char *)buf, count, 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
            if (ret <= 0)
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
            }
            return (ret > 0);
        }

        int beenWritten = 0;
        while (beenWritten < count)
        {
            ret = send(_sock, (char *)buf, count, 0);
            if (ret <= 0)
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return false;
            }
            beenWritten += ret;
        }

        return true;
    }

    void ConnClass::readAsync(int count, uint8_t *buf, void (*handler)(int count, uint8_t *buf, void *ctx), void *ctx, bool enforceSize)
    {
        if (!connectionOpen)
        {
            return;
        }
        // Create entry
        ConnReadEntry entry;
        entry.count = count;
        entry.buf = buf;
        entry.handler = handler;
        entry.ctx = ctx;
        entry.enforceSize = enforceSize;

        // Add entry to queue
        {
            std::lock_guard lck(readQueueMtx);
            readQueue.push_back(entry);
        }

        // Notify read worker
        readQueueCnd.notify_all();
    }

    void ConnClass::writeAsync(int count, uint8_t *buf)
    {
        if (!connectionOpen)
        {
            return;
        }
        // Create entry
        ConnWriteEntry entry;
        entry.count = count;
        entry.buf = buf;

        // Add entry to queue
        {
            std::lock_guard lck(writeQueueMtx);
            writeQueue.push_back(entry);
        }

        // Notify write worker
        writeQueueCnd.notify_all();
    }

    void ConnClass::readWorker()
    {
        while (true)
        {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(readQueueMtx);
            readQueueCnd.wait(lck, [this]()
                              { return (readQueue.size() > 0 || stopWorkers); });
            if (stopWorkers || !connectionOpen)
            {
                return;
            }

            // Pop first element off the list
            ConnReadEntry entry = readQueue[0];
            readQueue.erase(readQueue.begin());
            lck.unlock();

            // Read from socket and send data to the handler
            int ret = read(entry.count, entry.buf, entry.enforceSize);
            if (ret <= 0)
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return;
            }
            entry.handler(ret, entry.buf, entry.ctx);
        }
    }

    void ConnClass::writeWorker()
    {
        while (true)
        {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(writeQueueMtx);
            writeQueueCnd.wait(lck, [this]()
                               { return (writeQueue.size() > 0 || stopWorkers); });
            if (stopWorkers || !connectionOpen)
            {
                return;
            }

            // Pop first element off the list
            ConnWriteEntry entry = writeQueue[0];
            writeQueue.erase(writeQueue.begin());
            lck.unlock();

            // Write to socket
            if (!write(entry.count, entry.buf))
            {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return;
            }
        }
    }

    ListenerClass::ListenerClass(Socket listenSock)
    {
        sock = listenSock;
        listening = true;
        acceptWorkerThread = std::thread(&ListenerClass::worker, this);
    }

    ListenerClass::~ListenerClass()
    {
        close();
    }

    Conn ListenerClass::accept()
    {
        if (!listening)
        {
            return NULL;
        }
        std::lock_guard lck(acceptMtx);
        Socket _sock;

        // Accept socket
        _sock = ::accept(sock, NULL, NULL);
#ifdef _WIN32
        if (_sock < 0 || _sock == SOCKET_ERROR)
        {
#else
        if (_sock < 0)
        {
#endif
            listening = false;
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        return Conn(new ConnClass(_sock));
    }

    void ListenerClass::acceptAsync(void (*handler)(Conn conn, void *ctx), void *ctx)
    {
        if (!listening)
        {
            return;
        }
        // Create entry
        ListenerAcceptEntry entry;
        entry.handler = handler;
        entry.ctx = ctx;

        // Add entry to queue
        {
            std::lock_guard lck(acceptQueueMtx);
            acceptQueue.push_back(entry);
        }

        // Notify write worker
        acceptQueueCnd.notify_all();
    }

    void ListenerClass::close()
    {
        {
            std::lock_guard lck(acceptQueueMtx);
            stopWorker = true;
        }
        acceptQueueCnd.notify_all();

        if (listening)
        {
#ifdef _WIN32
            closesocket(sock);
#else
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
#endif
        }

        if (acceptWorkerThread.joinable())
        {
            acceptWorkerThread.join();
        }

        listening = false;
    }

    bool ListenerClass::isListening()
    {
        return listening;
    }

    void ListenerClass::worker()
    {
        while (true)
        {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(acceptQueueMtx);
            acceptQueueCnd.wait(lck, [this]()
                                { return (acceptQueue.size() > 0 || stopWorker); });
            if (stopWorker || !listening)
            {
                return;
            }

            // Pop first element off the list
            ListenerAcceptEntry entry = acceptQueue[0];
            acceptQueue.erase(acceptQueue.begin());
            lck.unlock();

            // Read from socket and send data to the handler
            try
            {
                Conn client = accept();
                if (!client)
                {
                    listening = false;
                    return;
                }
                entry.handler(std::move(client), entry.ctx);
            }
            catch (const std::exception &e)
            {
                listening = false;
                return;
            }
        }
    }

    Conn connect(std::string host, uint16_t port)
    {
        Socket sock;

#ifdef _WIN32
        // Initialize WinSock2
        if (!winsock_init)
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa))
            {
                throw std::runtime_error("Could not initialize WinSock2");
                return NULL;
            }
            winsock_init = true;
        }
        assert(winsock_init);
#else
        signal(SIGPIPE, SIG_IGN);
#endif

        // Create a socket
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
        {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

        // Get address from hostname/ip
        hostent *remoteHost = gethostbyname(host.c_str());
        if (remoteHost == NULL || remoteHost->h_addr_list[0] == NULL)
        {
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t *naddr = (uint32_t *)remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = *naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Connect to host
        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            throw std::runtime_error("Could not connect to host");
            return NULL;
        }

        return Conn(new ConnClass(sock));
    }

    Listener listen(std::string host, uint16_t port)
    {
        Socket listenSock;

#ifdef _WIN32
        // Initialize WinSock2
        if (!winsock_init)
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa))
            {
                throw std::runtime_error("Could not initialize WinSock2");
                return NULL;
            }
            winsock_init = true;
        }
        assert(winsock_init);
#else
        signal(SIGPIPE, SIG_IGN);
#endif

        // Create a socket
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock < 0)
        {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

#ifndef _WIN32
        // Allow port reusing if the app was killed or crashed
        // and the socket is stuck in TIME_WAIT state.
        // This option has a different meaning on Windows,
        // so we use it only for non-Windows systems
        int enable = 1;
        if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        {
            throw std::runtime_error("Could not configure socket");
            return NULL;
        }
#endif

        // Get address from hostname/ip
        hostent *remoteHost = gethostbyname(host.c_str());
        if (remoteHost == NULL || remoteHost->h_addr_list[0] == NULL)
        {
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t *naddr = (uint32_t *)remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = *naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Bind socket
        if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        // Listen
        if (::listen(listenSock, SOMAXCONN) != 0)
        {
            throw std::runtime_error("Could not listen");
            return NULL;
        }

        return Listener(new ListenerClass(listenSock));
    }

    Conn openUDP(std::string host, uint16_t port, std::string remoteHost, uint16_t remotePort, bool bindSocket)
    {
        Socket sock;

#ifdef _WIN32
        // Initialize WinSock2
        if (!winsock_init)
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa))
            {
                throw std::runtime_error("Could not initialize WinSock2");
                return NULL;
            }
            winsock_init = true;
        }
        assert(winsock_init);
#else
        signal(SIGPIPE, SIG_IGN);
#endif

        // Create a socket
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0)
        {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

        // Get address from local hostname/ip
        hostent *_host = gethostbyname(host.c_str());
        if (_host == NULL || _host->h_addr_list[0] == NULL)
        {
            throw std::runtime_error("Could get address from host");
            return NULL;
        }

        // Get address from remote hostname/ip
        hostent *_remoteHost = gethostbyname(remoteHost.c_str());
        if (_remoteHost == NULL || _remoteHost->h_addr_list[0] == NULL)
        {
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t *rnaddr = (uint32_t *)_remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = INADDR_ANY; //*naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Create remote host address
        struct sockaddr_in raddr;
        raddr.sin_addr.s_addr = *rnaddr;
        raddr.sin_family = AF_INET;
        raddr.sin_port = htons(remotePort);

        // Bind socket
        if (bindSocket)
        {
            int err = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
            if (err < 0)
            {
                throw std::runtime_error("Could not bind socket");
                return NULL;
            }
        }

        return Conn(new ConnClass(sock, raddr, true));
    }

    // DMH ============================================================
    std::string listenUDP_test(std::string host, uint16_t port, int sec)
    {
        Socket listenSock = -1; // Инициализируем как невалидный
        signal(SIGPIPE, SIG_IGN);

        // 1. Создание сокета
        listenSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listenSock < 0)
        {
            flog::error("Could not create socket: {0}", strerror(errno));
            return "error_socket_create";
        }
        /*
        // 2. Установка опции SO_REUSEADDR для быстрого перезапуска
        int optval = 1;
        if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        {
            flog::error("setsockopt(SO_REUSEADDR) failed: {0}", strerror(errno));
            close(listenSock);
            return "error_setsockopt";
        }
        */
        // 3. Создание адреса для привязки (bind)
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        // Привязываемся ко всем доступным интерфейсам
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        // 4. Привязка сокета к адресу и порту
        if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            flog::error("Binding to port {0} failed: {1}", port, strerror(errno));
            close(listenSock);
            return "error_bind";
        }

        // 5. Установка таймаута на получение данных
        timeval timeout = {
            .tv_sec = sec,
            .tv_usec = 0};
        if (setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval)) < 0)
        {
            flog::error("setsockopt(SO_RCVTIMEO) failed: {0}", strerror(errno));
            close(listenSock);
            return "error_timeout";
        }

        // 6. Получение данных с обработкой прерываний (EINTR)
        char buf[1024];
        struct sockaddr_in remoteAddr;
        socklen_t fromLen = sizeof(remoteAddr);
        int ret;
        std::string outString = "error_recv"; // Значение по умолчанию

        flog::info("Port {0}: Entering blocking recvfrom loop for {1} seconds...", port, sec);

        do
        {
            ret = recvfrom(listenSock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&remoteAddr, &fromLen);

            // Если вызов был прерван сигналом, просто пробуем снова
            if (ret < 0 && errno == EINTR)
            {
                flog::warn("Port {0}: recvfrom was interrupted by a signal. Retrying...", port);
                continue; // Возвращаемся к началу цикла
            }

            // Если была любая другая ошибка или успех, выходим из цикла
            break;

        } while (true);

        flog::info("Port {0}: Exited recvfrom loop.", port);

        // Анализируем результат после выхода из цикла
        if (ret < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                flog::warn("Timeout: Message from ACF/VA service not received on port {0} within {1} seconds.", port, sec);
                outString = "error_timeout";
            }
            else
            {
                // Другая, не-EINTR и не-таймаут ошибка
                flog::error("recvfrom failed with error: {0}", strerror(errno));
            }
        }
        else if (ret == 0)
        {
            // Это маловероятно для UDP, но для полноты картины
            flog::warn("recvfrom returned 0 bytes.");
        }
        else
        {
            buf[ret] = 0; // Корректное завершение строки
            std::string str(buf);
            flog::info("recvfrom received {0} bytes: '{1}'", ret, str);
            outString = str;
        }

        // 7. Закрытие сокета
        ::shutdown(listenSock, SHUT_RDWR);
        ::close(listenSock);

        return outString;
    }

    std::string listenUDP_old(std::string host, uint16_t port, int sec, int interrupt_fd = -1)
    {
        // https://stackoverflow.com/questions/14665543/how-do-i-receive-udp-packets-with-winsock-in-c
        // https://stackoverflow.com/questions/60966657/how-to-set-timeout-in-udp-socket-with-c-c-in-windows

        Socket listenSock;
        signal(SIGPIPE, SIG_IGN);
        std::condition_variable connectionOpenCnd;
        // Create a socket
        listenSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listenSock < 0)
        {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

        // 2. Установка опции SO_REUSEADDR для быстрого перезапуска
        /*
        int optval = 1;
        if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        {
            flog::error("setsockopt(SO_REUSEADDR) failed: {0}", strerror(errno));
            close(listenSock);
            return "error_setsockopt";
        }
        */

        // Get address from hostname/ip
        hostent *remoteHost = gethostbyname(host.c_str());
        if (remoteHost == NULL || remoteHost->h_addr_list[0] == NULL)
        {
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t *naddr = (uint32_t *)remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = *naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            flog::info("binding");
        }
        else
        {
            flog::info("binding {0}:{1}", host, port);
        }
        timeval timeout = {
            .tv_sec = sec,
            .tv_usec = 0};
        // timeout_set = true;
        setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval));
        std::string OUT_STRNG = "error";

        flog::info("Port {0}: >>> Entering recvfrom, expecting to wait up to {1} seconds.", port, sec);
        auto startTime = std::chrono::steady_clock::now();
        int length, n;
        char buf[1024];
        // unsigned short serverPort = 27072;
        struct sockaddr_in remoteAddr;
        int count = 1024;
        socklen_t fromLen = sizeof(remoteAddr);
        int ret;
        ret = recvfrom(listenSock, (char *)buf, count, 0, (struct sockaddr *)&remoteAddr, &fromLen);
        // ret = recv(listenSock, (char*)buf, count, 0);
        if (ret <= 0)
        {
            {
                flog::warn("Message from ACF/VA service not received");
                // std::lock_guard lck(connectionOpenMtx);
                // connectionOpen = false;
            }
            // connectionOpenCnd.notify_all();
        }
        else
        {
            buf[ret - 1] = 0;
            std::string str(buf);
            flog::info("recvfrom {0}. Received a datagram: {1}", ret, str);
            OUT_STRNG = str;
        }
        auto endTime = std::chrono::steady_clock::now();
        long durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        flog::info("Port {0}: >>> Exited recvfrom. Actual wait time: {1} ms.", port, durationMs);

        // if (connectionOpen) {
        ::shutdown(listenSock, SHUT_RDWR);
        ::close(listenSock);
        // }
        return OUT_STRNG;
    }

    // Новая, безопасная версия listenUDP с возможностью прерывания и очисткой пайпа
    std::string listenUDP(std::string host, uint16_t port, int sec, int interrupt_fd = -1)
    {
        using Socket = int;
        signal(SIGPIPE, SIG_IGN);

        // Нормализуем host
        auto normalize_host = [](const std::string &h) -> in_addr
        {
            in_addr out{};
            if (h.empty() || h == "0.0.0.0")
            {
                out.s_addr = htonl(INADDR_ANY);
                return out;
            }
            if (h == "localhost")
            {
                inet_pton(AF_INET, "127.0.0.1", &out);
                return out;
            }
            if (inet_pton(AF_INET, h.c_str(), &out) == 1)
            { // ok
                return out;
            }
            // Фолбэк
            flog::warn("listenUDP: inet_pton failed for host '{0}', falling back to 127.0.0.1", h.c_str());
            inet_pton(AF_INET, "127.0.0.1", &out);
            return out;
        };

        const in_addr bind_addr = normalize_host(host);

        auto make_socket_and_bind = [&](Socket &udpSocket) -> bool
        {
            udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udpSocket < 0)
            {
                flog::error("listenUDP: socket() failed: {0}", strerror(errno));
                return false;
            }

            int one = 1;
            if (setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
            {
                flog::warn("listenUDP: setsockopt(SO_REUSEADDR) failed: {0}", strerror(errno));
            }

            // увеличить recv буфер
            int want_rcv = 1 << 20; // 1MB
            (void)setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &want_rcv, sizeof(want_rcv));
            int eff_rcv = 0;
            socklen_t sl = sizeof(eff_rcv);
            if (getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &eff_rcv, &sl) == 0)
            {
                // flog::info("listenUDP: effective SO_RCVBUF={0}", eff_rcv);
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr = bind_addr; // 127.0.0.1 или INADDR_ANY или заданный IP

            if (bind(udpSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            {
                flog::error("listenUDP: bind({0}) failed: {1}", port, strerror(errno));
                close(udpSocket);
                udpSocket = -1;
                return false;
            }

            // Лог реального адреса
            socklen_t alen = sizeof(addr);
            if (getsockname(udpSocket, (struct sockaddr *)&addr, &alen) == 0)
            {
                char ipbuf[64]{};
                inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
                // flog::info("listenUDP: bound to {0}:{1}", ipbuf, ntohs(addr.sin_port));
            }

            return true;
        };

        // До двух попыток ожидания (каждая по sec)
        const int attempts = 1;
        for (int attempt = 1; attempt <= attempts; ++attempt)
        {
            Socket udpSocket = -1;
            if (!make_socket_and_bind(udpSocket))
            {
                return "error_bind";
            }

            flog::info("listenUDP: Waiting on {0}:{1} (interrupt fd: {2}) for {3}s (attempt {4}/{5})...",
                       host.empty() ? "127.0.0.1" : host.c_str(), port, interrupt_fd, sec, attempt, attempts);

            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(udpSocket, &read_fds);
            int max_fd = udpSocket;

            if (interrupt_fd != -1)
            {
                FD_SET(interrupt_fd, &read_fds);
                max_fd = std::max(udpSocket, interrupt_fd);
            }

            timeval timeout{};
            timeout.tv_sec = sec;
            timeout.tv_usec = 0;

            int activity = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);

            if (activity < 0)
            {
                int e = errno;
                close(udpSocket);
                if (e == EINTR)
                {
                    flog::warn("listenUDP: select() interrupted by system signal.");
                    return "interrupted";
                }
                flog::error("listenUDP: select() error: {0}", strerror(e));
                return "error_select";
            }

            if (activity == 0)
            {
                flog::warn("listenUDP: Timeout after {0} seconds on port {1} (attempt {2}/{3}).",
                           sec, port, attempt, attempts);
                close(udpSocket);
                if (attempt < attempts)
                    continue; // следующая попытка
                return "error_timeout";
            }

            if (interrupt_fd != -1 && FD_ISSET(interrupt_fd, &read_fds))
            {
                // слить сигнальные байты
                char drain[32];
                while (read(interrupt_fd, drain, sizeof(drain)) > 0)
                {
                }
                close(udpSocket);
                flog::warn("listenUDP: Interrupted by signal pipe (fd={0}).", interrupt_fd);
                return "interrupted";
            }

            if (FD_ISSET(udpSocket, &read_fds))
            {
                char buf[2048];
                sockaddr_in remote{};
                socklen_t rlen = sizeof(remote);
                int ret = recvfrom(udpSocket, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&remote, &rlen);
                int saved_errno = errno;
                close(udpSocket);

                if (ret < 0)
                {
                    if (saved_errno == EINTR)
                    {
                        flog::warn("listenUDP: recvfrom interrupted (EINTR).");
                        return "interrupted";
                    }
                    flog::error("listenUDP: recvfrom error: {0}", strerror(saved_errno));
                    return "error_recv";
                }
                if (ret == 0)
                {
                    // пустая датаграмма
                    return std::string();
                }
                buf[ret] = '\0';
                return std::string(buf);
            }

            // Теоретически недостижимо
            close(udpSocket);
        }

        return "error_unknown";
    }
}