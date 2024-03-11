#include "main.h"

using namespace std;
//======================================================================
static mutex mtx_conn;
static condition_variable cond_close_conn;
static int num_conn = 0;
//======================================================================
void start_conn()
{
mtx_conn.lock();
    ++num_conn;
mtx_conn.unlock();
}
//======================================================================
void wait_close_all_conn()
{
unique_lock<mutex> lk(mtx_conn);
    while (num_conn > 0)
    {
        cond_close_conn.wait(lk);
    }
}
//======================================================================
int is_maxconn()
{
mtx_conn.lock();
    int n = 0;
    if (num_conn >= conf->MaxWorkConnections)
        n = 1;
mtx_conn.unlock();
    return n;
}
//======================================================================
void close_connect(Connect *req)
{
    if ((req->Protocol == HTTPS) && (req->tls.ssl))
    {
        SSL_free(req->tls.ssl);
    }

    shutdown(req->clientSocket, SHUT_RDWR);
    close(req->clientSocket);
    delete req;

mtx_conn.lock();
    --num_conn;
mtx_conn.unlock();
    cond_close_conn.notify_all();
}
//======================================================================
void end_response(Connect *req)
{
    if ((req->connKeepAlive == 0) || (req->err < 0))
    { // ----- Close connect -----
        if (req->err <= -RS101)
        {
            req->respStatus = -req->err;
            req->err = 0;
            req->hdrs = "";
            if (send_message(req, NULL) == 1)
                return;
        }

        if ((req->operation != SSL_ACCEPT) && 
            (req->operation != READ_REQUEST))
        {
            print_log(req);
        }

        if ((req->Protocol == HTTPS) && (req->tls.ssl))
        {
    #ifdef TCP_CORK_
            if (conf->TcpCork == 'y')
            {
            #if defined(LINUX_)
                int optval = 0;
                setsockopt(req->clientSocket, SOL_TCP, TCP_CORK, &optval, sizeof(optval));
            //#elif defined(FREEBSD_)
            #else
                int optval = 0;
                setsockopt(req->clientSocket, IPPROTO_TCP, TCP_NOPUSH, &optval, sizeof(optval));
            #endif
            }
    #endif
            if ((req->tls.err != SSL_ERROR_SSL) && 
                (req->tls.err != SSL_ERROR_SYSCALL))
            {
                int ret = SSL_shutdown(req->tls.ssl);
                if (ret == -1)
                {
                    req->tls.err = SSL_get_error(req->tls.ssl, ret);
                    print_err(req, "<%s:%d> Error SSL_shutdown()=%d, err=%s\n", __func__, __LINE__, ret, ssl_strerror(req->tls.err));
                    if (req->tls.err == SSL_ERROR_ZERO_RETURN)
                    {
                        close_connect(req);
                        return;
                    }
                    else if (req->tls.err == SSL_ERROR_WANT_READ)
                    {
                        req->event = POLLIN;
                        req->operation = SSL_SHUTDOWN;
                        push_pollin_list(req);
                        return;
                    }
                    else if (req->tls.err == SSL_ERROR_WANT_WRITE)
                    {
                        req->event = POLLOUT;
                        req->operation = SSL_SHUTDOWN;
                        push_pollin_list(req);
                        return;
                    }
                }
                else if (ret == 0)
                {
                    req->operation = SSL_SHUTDOWN;
                    push_pollin_list(req);
                    return;
                }
            }
            else
            {
                print_err(req, "<%s:%d> tls.err: %s\n", __func__, __LINE__, ssl_strerror(req->tls.err));
            }
        }

        close_connect(req);
    }
    else
    { // ----- KeepAlive -----
    #ifdef TCP_CORK_
        if (conf->TcpCork == 'y')
        {
        #if defined(LINUX_)
            int optval = 0;
            setsockopt(req->clientSocket, SOL_TCP, TCP_CORK, &optval, sizeof(optval));
        //#elif defined(FREEBSD_)
        #else
            int optval = 0;
            setsockopt(req->clientSocket, IPPROTO_TCP, TCP_NOPUSH, &optval, sizeof(optval));
        #endif
        }
    #endif
        print_log(req);
        req->init();
        req->timeout = conf->TimeoutKeepAlive;
        ++req->numReq;
        req->operation = READ_REQUEST;
        push_pollin_list(req);
    }
}
//======================================================================
static unsigned long allConn = 0;
static int nProc;
//======================================================================
static void signal_handler_child(int sig)
{
    if (sig == SIGINT)
    {
        print_err("[%d]<%s:%d> ### SIGINT ### all_conn=%lu, open_conn=%d\n", nProc,
                    __func__, __LINE__, allConn, num_conn);
    }
    else if (sig == SIGTERM)
    {
        print_err("[%d]<%s:%d> ### SIGTERM ###\n", nProc, __func__, __LINE__);
        exit(0);
    }
    else if (sig == SIGSEGV)
    {
        print_err("[%d]<%s:%d> ### SIGSEGV ###\n", nProc, __func__, __LINE__);
        exit(1);
    }
    else if (sig == SIGUSR1)
        print_err("[%d]<%s:%d> ### SIGUSR1 ###\n", nProc, __func__, __LINE__);
    else if (sig == SIGUSR2)
        print_err("[%d]<%s:%d> ### SIGUSR2 ###\n", nProc, __func__, __LINE__);
    else
        print_err("[%d]<%s:%d> sig=%d\n", nProc, __func__, __LINE__, sig);
}
//======================================================================
Connect *create_req();
int write_(int fd, void *data, int size);
int read_(int fd, void *data, int size);
void close_threads_manager();
void threads_manager(int numProc);
unsigned long get_all_request();
int get_max_thr();
//======================================================================
void manager(int sockServer, unsigned int numProc, int fd_in, int fd_out, char sig)
{
    nProc = numProc;
    //------------------------------------------------------------------
    if (signal(SIGINT, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGINT): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGSEGV, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGSEGV): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGTERM, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGTERM): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGUSR1, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGUSR1): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGUSR2, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGUSR2): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    if (chdir(conf->DocumentRoot.c_str()))
    {
        print_err("[%d]<%s:%d> Error chdir(%s): %s\n", numProc, __func__, __LINE__, conf->DocumentRoot.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    thread EventHandler;
    try
    {
        EventHandler = thread(event_handler, numProc);
    }
    catch (...)
    {
        print_err("[%d]<%s:%d> Error create thread(event_handler): errno=%d\n", numProc, __func__, __LINE__, errno);
        exit(errno);
    }
    //------------------------------------------------------------------
    thread thr;
    try
    {
        thr = thread(threads_manager, numProc);
    }
    catch (...)
    {
        print_err("<%s:%d> Error create thread: errno=%d\n", __func__, __LINE__, errno);
        exit(errno);
    }
    //------------------------------------------------------------------
    printf("[%u] +++++ pid=%u, uid=%u, gid=%u +++++\n", numProc, getpid(), getuid(), getgid());
    //------------------------------------------------------------------
    static struct pollfd fdrd[2];
    fdrd[0].fd = fd_in;
    fdrd[0].events = POLLIN;

    fdrd[1].fd = sockServer;
    fdrd[1].events = POLLIN;

    unsigned char status = sig;
    int num_fd = 1, run = 1;
    if (write_(fd_out, &status, sizeof(status)) < 0)
        run = 0;

    while (run)
    {
        struct sockaddr_storage clientAddr;
        socklen_t addrSize = sizeof(struct sockaddr_storage);

        if ((0x7f & status) == CONNECT_ALLOW)
        {
            if (is_maxconn())
            {//------ the number of connections is the maximum ---------
                // Allow connections next worker process
                if (write_(fd_out, &status, sizeof(status)) < 0)
                    break;
                status = CONNECT_IGN;
                num_fd = 1;// poll(): pipe[in]
            }
            else
            {
                num_fd = 2;// poll(): pipe[in] and listen socket
                status = CONNECT_ALLOW;
            }
        }
        else
            num_fd = 1;// poll(): pipe[in]

        int ret_poll = poll(fdrd, num_fd, -1);
        if (ret_poll <= 0)
        {
            if (errno == EINTR)
                continue;
            print_err("[%d]<%s:%d> Error poll()=-1: %s\n", numProc, __func__, __LINE__, strerror(errno));
            break;
        }

        if (fdrd[0].revents == POLLIN)
        {
            --ret_poll;
            unsigned char ch;
            if (read_(fd_in, &ch, sizeof(ch)) <= 0)
            {
                print_err("[%d]<%s:%d> Error read(): %s\n", numProc, __func__, __LINE__, strerror(errno));
                break;
            }

            if (ch == PROC_CLOSE)
            {
                // Close next process
                write_(fd_out, &ch, sizeof(ch));
                break;
            }

            status = ch;
            continue;
        }

        if (fdrd[1].revents == POLLIN)
        {
            --ret_poll;

            int clientSocket = accept(sockServer, (struct sockaddr *)&clientAddr, &addrSize);
            if (clientSocket == -1)
            {
                if ((errno == EAGAIN) || (errno == EINTR) || (errno == EMFILE))
                    continue;
                break;
                print_err("[%d]<%s:%d>  Error accept(): %s\n", numProc, __func__, __LINE__, strerror(errno));
            }

            Connect *req;
            req = create_req();
            if (!req)
            {
                shutdown(clientSocket, SHUT_RDWR);
                close(clientSocket);
                break;
            }

            int flags = 1;
            if (ioctl(clientSocket, FIONBIO, &flags) == -1)
            {
                print_err("<%s:%d> Error ioctl(FIONBIO, 1): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            if (ioctl(clientSocket, FIOCLEX) == -1)
            {
                print_err("<%s:%d> Error ioctl(FIOCLEX): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }
/*
            flags = fcntl(clientSocket, F_GETFD);
            if (flags == -1)
            {
                print_err("<%s:%d> Error fcntl(F_GETFD): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            flags |= FD_CLOEXEC;
            if (fcntl(clientSocket, F_SETFD, flags) == -1)
            {
                print_err("<%s:%d> Error fcntl(F_SETFD, FD_CLOEXEC): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }
*/
            req->init();
            req->numProc = numProc;
            req->numConn = ++allConn;
            req->numReq = 1;
            req->Protocol = conf->Protocol;
            req->serverSocket = sockServer;
            req->clientSocket = clientSocket;
            req->timeout = conf->Timeout;

            int err;
            if ((err = getnameinfo((struct sockaddr *)&clientAddr,
                    addrSize,
                    req->remoteAddr,
                    sizeof(req->remoteAddr),
                    req->remotePort,
                    sizeof(req->remotePort),
                    NI_NUMERICHOST | NI_NUMERICSERV)))
            {
                print_err(req, "<%s:%d> Error getnameinfo()=%d: %s\n", __func__, __LINE__, err, gai_strerror(err));
                req->remoteAddr[0] = 0;
            }

            if (req->Protocol == HTTPS)
            {
                req->tls.err = 0;
                req->tls.ssl = SSL_new(conf->ctx);
                if (!req->tls.ssl)
                {
                    print_err(req, "<%s:%d> Error SSL_new()\n", __func__, __LINE__);
                    shutdown(clientSocket, SHUT_RDWR);
                    close(clientSocket);
                    delete req;
                    break;
                }

                int ret = SSL_set_fd(req->tls.ssl, req->clientSocket);
                if (ret == 0)
                {
                    req->tls.err = SSL_get_error(req->tls.ssl, ret);
                    print_err(req, "<%s:%d> Error SSL_set_fd(): %s\n", __func__, __LINE__, ssl_strerror(req->tls.err));
                    SSL_free(req->tls.ssl);
                    shutdown(clientSocket, SHUT_RDWR);
                    close(clientSocket);
                    delete req;
                    continue;
                }

                req->operation = SSL_ACCEPT;
            }
            else
            {
                req->operation = READ_REQUEST;
            }

            start_conn();
            push_pollin_list(req);
        }

        if (ret_poll)
        {
            print_err("[%d]<%s:%d>  Error: revents=0x%x; socket revents=0x%x\n",
                        numProc, __func__, __LINE__, fdrd[0].revents, fdrd[1].revents);
            break;
        }

        if (conf->BalancedLoad == 'y')
        {
            status = CONNECT_IGN;
            char ch = CONNECT_ALLOW;
            if (write_(fd_out, &ch, sizeof(ch)) < 0)
                break;
        }
    }

    wait_close_all_conn();

    close(sockServer);

    print_err("[%d]<%s:%d> all_req=%u; open_conn=%d, status=%u\n", numProc,
                    __func__, __LINE__, get_all_request(), num_conn, (unsigned int)status);
    close_event_handler();
    EventHandler.join();

    close_threads_manager();
    thr.join();

    usleep(100000);
    close(fd_out);
}
//======================================================================
Connect *create_req(void)
{
    Connect *req = new(nothrow) Connect;
    if (!req)
        print_err("<%s:%d> Error malloc(): %s\n", __func__, __LINE__, strerror(errno));
    return req;
}
//======================================================================
int write_(int fd, void *data, int size)
{
    int ret, err;
a1:
    ret = write(fd, data, size);
    if (ret < 0)
    {
        err = errno;
        if (err == EINTR)
            goto a1;
        print_err("<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(err));
    }
    return ret;
}
//======================================================================
int read_(int fd, void *data, int size)
{
    int ret, err;
a1:
    ret = read(fd, data, size);
    if (ret < 0)
    {
        err = errno;
        if (err == EINTR)
            goto a1;
        print_err("<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(err));
    }

    return ret;
}
