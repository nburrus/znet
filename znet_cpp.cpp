#include "znet_cpp.h"

# define ZN_API static inline
# define ZN_IMPLEMENTATION
#include "znet.h"

#include <cassert>
#include <thread>
#include <vector>
#include <iostream>

namespace znet {

    static void lineTcpClientOnConnection(void *ud, zn_Tcp *tcp, unsigned err);
    static void lineTcpClientOnMessageSent(void *ud, zn_Tcp *tcp, unsigned err, unsigned count);
    static void lineTcpClientOnRecv(void *ud, zn_Tcp *tcp, unsigned err, unsigned count);

    struct LineTcpClient::Impl
    {
        LineReceivedCallback lineReceivedCb = nullptr;
        ClientConnectedCallback clientConnectedCb = nullptr;
        std::atomic_bool connected { false };
        std::string serverIp;
        int serverPort = -1;
        
        zn_Tcp *tcp = nullptr;
        zn_State* state = nullptr;

        std::vector<char> recvBuffer;
        std::string currentLine;

        int numConnectionAttempts = 0;

        void onConnection (unsigned err)
        {
            if (err != ZN_OK)
            { /* no lucky? let's try again. */
                /* we use ud to find out which time we tried. */
                fprintf(stderr, "[%p] client can not connect to server now: %s\n",
                        tcp, zn_strerror(err));
                if (++numConnectionAttempts < 10)
                {
                    fprintf(stderr, "[%p client trying again (%d times)! :-/ \n",
                            tcp, numConnectionAttempts);
                    zn_connect(tcp, serverIp.c_str(), serverPort, lineTcpClientOnConnection, this);
                }
                else
                {
                    fprintf(stderr, "[%p] client just gave up to connect :-( \n", tcp);
                    zn_deltcp(tcp);
                }
                return;
            }

            recvBuffer.resize (2048);
            zn_recv(tcp, recvBuffer.data(), recvBuffer.size(), lineTcpClientOnRecv, this);

            fprintf(stderr, "[%p] client connected to server now!\n", tcp);
            connected = true;

            if (clientConnectedCb)
                clientConnectedCb();
        }

        void onMessageSent(unsigned err, unsigned count)
        {
            /* send work may error out, we first check the result code: */
            if (err != ZN_OK)
            {
                fprintf(stderr, "[%p] client error when sending something: %s\n",
                        tcp, zn_strerror(err));
                zn_deltcp(tcp); /* and we close connection. */
                return;
            }
        }

        void onMessageReceived(unsigned err, unsigned count)
        {
            if (err != ZN_OK)
            {
                fprintf(stderr, "[%p] client error when receiving something: %s\n",
                        tcp, zn_strerror(err));
                zn_deltcp(tcp); /* and we close connection. */
                return;
            }

            std::cerr << "count = " << count << std::endl;
            std::string str (recvBuffer.begin(), recvBuffer.end());
            // fprintf (stderr, "Received string %s (count=%d)\n", str.c_str(), count);

            auto bufferStartIt = recvBuffer.begin();
            do {
                auto newLineIt = std::find (bufferStartIt, recvBuffer.end(), '\n');
                if (newLineIt != recvBuffer.end())
                    ++newLineIt; // include the \n

                currentLine.append (bufferStartIt, newLineIt);
                if (newLineIt != recvBuffer.end())
                {
                    if (lineReceivedCb)
                        lineReceivedCb(currentLine);
                    currentLine.clear ();
                }

                bufferStartIt = newLineIt;
            } while (bufferStartIt != recvBuffer.end());

            zn_recv(tcp, recvBuffer.data(), recvBuffer.size(), lineTcpClientOnRecv, this);
        }
    };

    LineTcpClient::LineTcpClient()
    : d (new Impl())
    {}

    LineTcpClient::~LineTcpClient () = default;

    /* the client connection callback: when you want to connect other
    * server, and it's done, this function will be called. */
    static void lineTcpClientOnConnection(void *ud, zn_Tcp *tcp, unsigned err) {
        LineTcpClient::Impl *impl = (LineTcpClient::Impl*)ud;
        impl->onConnection (err);        
    }

    static void lineTcpClientOnMessageSent(void *ud, zn_Tcp *tcp, unsigned err, unsigned count) {
        LineTcpClient::Impl *impl = (LineTcpClient::Impl*)ud;
        impl->onMessageSent (err, count);
    }

    static void lineTcpClientOnRecv(void *ud, zn_Tcp *tcp, unsigned err, unsigned count) {
        LineTcpClient::Impl *impl = (LineTcpClient::Impl*)ud;
        impl->onMessageReceived (err, count);
    }

    void LineTcpClient::setLineReceivedCallback (LineReceivedCallback cb)
    {
        assert (!d->connected); // Cannot assign it once connected, not thread-safe.
        if (!d->connected)
            d->lineReceivedCb = cb;
    }

    void LineTcpClient::setClientConnectedCallback (ClientConnectedCallback cb)
    {
        assert (!d->connected); // Cannot assign it once connected, not thread-safe.        
        if (!d->connected)
            d->clientConnectedCb = cb;
    }

    bool LineTcpClient::connectToServer (const std::string& serverIp, int port)
    {
        zn_initialize(); // safe to call multiple times.
        d->state = zn_newstate();
        if (!d->state) {
            fprintf(stderr, "[ZNET] create handler failed\n");
            return false;
        }
        d->tcp = zn_newtcp(d->state);
        zn_connect(d->tcp, serverIp.c_str(), port, lineTcpClientOnConnection, d.get());
        return false;
    }

    bool LineTcpClient::disconnect ()
    {
        zn_close(d->state);
        zn_deinitialize();
        return false;
    }

    bool LineTcpClient::sendString (const std::string& str)
    {
        zn_send(d->tcp, str.c_str(), str.size(), lineTcpClientOnMessageSent, d.get());
        return false;
    }

    void LineTcpClient::waitUntilConnected ()
    {
        while (!d->connected)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void LineTcpClient::runLoop ()
    {
        int err = zn_run(d->state, ZN_RUN_LOOP);
        if (err != ZN_OK)
        {
            fprintf(stderr, "[%p] client runloop finished: %s\n",
                    d->tcp, zn_strerror(err));
        }
    }

    // class LineTcpServer
    // {
    // public:
    //     void setLineReceivedCallback (LineReceivedCallback cb);

    // public:
    //     bool startListening (int port);
    //     bool disconnect ();
    //     bool sendString (const std::string& str);

    // private:
    //     struct Impl; friend struct impl;
    //     std::unique_ptr<Impl> d;
    // };

} // znet

int main()
{
    znet::LineTcpClient tcpClient;
    
    tcpClient.setLineReceivedCallback ([](const std::string& str) {
        std::cerr << "Received line: " << str;
    });

    tcpClient.setClientConnectedCallback ([&tcpClient]() {
        std::cerr << "Connected!" << std::endl;
        tcpClient.sendString("hello\n");
    });

    std::thread t ([&tcpClient](){
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            tcpClient.sendString ("proutFromThread");
        }
    });

    tcpClient.connectToServer ("127.0.0.1", 4999);
    tcpClient.runLoop ();
}
