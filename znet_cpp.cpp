#include "znet_cpp.h"

# define ZN_API static inline
# define ZN_IMPLEMENTATION
#include "znet.h"

#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <sstream>
#include <chrono>
#include <cmath>

namespace znet {
    
    template <class ImplT>
    struct TcpSendData
    {
        ImplT* impl;
        std::string str;
    };

    static void messageSenderOnMessageSent(void *ud, zn_Tcp *tcp, unsigned err, unsigned count);

    struct MessageSender
    {
        void start (zn_Tcp* tcp)
        {
            if (_tcp)
            {
                stop();
                fprintf (stderr, "Stopped before starting a new one.\n");
            }            

            _tcp = tcp;
            _shouldExit = false;
            _loopThread = std::thread([this]() {
                runLoop ();
            });
        }

        void stop ()
        {
            _shouldExit = true;
            _eventCondition.notify_one();
            _loopThread.join ();
            _tcp = nullptr;
            while (!_messagesToSend.empty()) _messagesToSend.pop();
        }

        bool sendString (const std::string& s)
        {
            std::unique_lock<std::mutex> _(_queueMutex);
            _messagesToSend.push (std::unique_ptr<std::string>(new std::string(s)));
            _eventCondition.notify_one();
            return true;
        }

    private:
        void runLoop ()
        {
            while (!_shouldExit)
            {
                std::unique_lock<std::mutex> lk(_queueMutex);
                _eventCondition.wait(lk, [this]() { 
                    return _shouldExit || (!_messagesToSend.empty() && !_hasMessageInFlight);
                });

                if (_shouldExit)
                    break;
                
                // Already busy sending messages, we'll keep flushing the queue
                // in the callback of zn_send.
                if (_hasMessageInFlight)
                    continue;

                if (!_hasMessageInFlight && !_messagesToSend.empty())
                {
                    sendNextMessage();
                }
            }

            while (_hasMessageInFlight)
            {
                fprintf(stderr, "Waiting for in-flight messages...\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        void sendNextMessage ()
        {
            _hasMessageInFlight = true;
            _numBytesSentFromFrontString = 0;
            auto* str = _messagesToSend.front().get();
            zn_send(_tcp,
                    str->c_str(),
                    (unsigned)str->size(),
                    messageSenderOnMessageSent,
                    this);
        }

    public: // fake public for the static function.
        void onDataSent (unsigned err, unsigned count)
        {
            /* send work may error out, we first check the result code: */
            if (err != ZN_OK)
            {
                fprintf(stderr, "[%p] client error when sending something: %s\n",
                        _tcp, zn_strerror(err));
                // zn_deltcp(_tcp); /* and we close connection. */
                _hasMessageInFlight = false;
                _shouldExit = true;
                _eventCondition.notify_one();
                return;
            }

            std::unique_lock<std::mutex> lk(_queueMutex);
            auto* str = _messagesToSend.front().get();
            _numBytesSentFromFrontString += count;
            if (_numBytesSentFromFrontString == (unsigned)str->size())
            {
                _messagesToSend.pop();
                _numBytesSentFromFrontString = 0;

                if (_messagesToSend.empty())
                {
                    _hasMessageInFlight = false;
                    _eventCondition.notify_one();
                }
                else
                {
                    sendNextMessage();
                }
            }
            else
            {
                // case where string was only sent partially not handled yet.
                assert(false);
            }
        }

    private:
        zn_Tcp* _tcp = nullptr;
        std::queue<std::unique_ptr<std::string>> _messagesToSend;
        unsigned _numBytesSentFromFrontString = 0;
        std::mutex _queueMutex;
        std::thread _loopThread;
        bool _shouldExit = false;
        std::condition_variable _eventCondition;
        std::atomic_bool _hasMessageInFlight { false };
    };

    static void messageSenderOnMessageSent(void *ud, zn_Tcp *tcp, unsigned err, unsigned count) {
        auto* sender = reinterpret_cast<MessageSender*>(ud);
        sender->onDataSent (err, count);
    }

    struct TcpLineReader
    {
        std::string currentLine;

        void processNewInputData(const std::vector<char> &recvBuffer,
                                 int count,
                                 LineReceivedCallback lineReceivedCb)
        {
            if (count < 1)
                return;

            auto bufferEndIt = recvBuffer.begin() + count;
            auto bufferStartIt = recvBuffer.begin();

            std::string debugStr (bufferStartIt, bufferEndIt);
            std::cerr << "inputStr = " << debugStr << std::endl;
            std::cerr << "inputStr.back() = " << (int)debugStr.back() << std::endl;

            do {
                bool foundEndOfLine = false;
                auto newLineIt = std::find (bufferStartIt, bufferEndIt, '\n');
                if (newLineIt != bufferEndIt)
                {
                    foundEndOfLine = true;
                    ++newLineIt; // include the \n
                }

                currentLine.append (bufferStartIt, newLineIt);
                if (foundEndOfLine)
                {
                    if (lineReceivedCb)
                        lineReceivedCb(currentLine);
                    currentLine.clear ();
                }

                bufferStartIt = newLineIt;
            } while (bufferStartIt != bufferEndIt);
        }
    };

} // znet

namespace znet {

    static void lineTcpClientOnConnection(void *ud, zn_Tcp *tcp, unsigned err);
    static void lineTcpClientOnRecv(void *ud, zn_Tcp *tcp, unsigned err, unsigned count);

    struct LineTcpClient::Impl
    {
        LineReceivedCallback lineReceivedCb = nullptr;
        ConnectedToServerCallback connectedToServerCb = nullptr;
        std::atomic_bool connected { false };
        std::string serverIp;
        int serverPort = -1;
        
        zn_Tcp *tcp = nullptr;
        zn_State* state = nullptr;

        std::vector<char> recvBuffer;
        std::string currentLine;

        int numConnectionAttempts = 0;

        TcpLineReader lineReader;

        MessageSender messageSender;

        void onConnection (unsigned err)
        {
            if (err != ZN_OK)
            { /* no lucky? let's try again. */
                /* we use ud to find out which time we tried. */
                fprintf(stderr, "[%p] client can not connect to server now: %s\n",
                        tcp, zn_strerror(err));
                if (++numConnectionAttempts < 10)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
            zn_recv(tcp, recvBuffer.data(), (unsigned)recvBuffer.size(), lineTcpClientOnRecv, this);

            messageSender.start (tcp);

            fprintf(stderr, "[%p] client connected to server now!\n", tcp);
            connected = true;

            if (connectedToServerCb)
                connectedToServerCb();
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

            std::cerr << "client received " << count << " bytes" << std::endl;
            lineReader.processNewInputData (recvBuffer, count, lineReceivedCb);
            zn_recv(tcp, recvBuffer.data(), (unsigned)recvBuffer.size(), lineTcpClientOnRecv, this);
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

    void LineTcpClient::setConnectedToServerCallback (ConnectedToServerCallback cb)
    {
        assert (!d->connected); // Cannot assign it once connected, not thread-safe.        
        if (!d->connected)
            d->connectedToServerCb = cb;
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
        d->serverIp = serverIp;
        d->serverPort = port;
        zn_connect(d->tcp, serverIp.c_str(), port, lineTcpClientOnConnection, d.get());
        return false;
    }

    bool LineTcpClient::disconnect ()
    {
        d->messageSender.stop ();
        zn_close(d->state);
        zn_deinitialize();
        return true;
    }

    bool LineTcpClient::sendString (const std::string& str)
    {
        if (!d->tcp)
        {
            fprintf (stderr, "Connection closed!\n");
            return false;
        }

        return d->messageSender.sendString (str);
    }

    void LineTcpClient::runLoop ()
    {
        int err = zn_run(d->state, ZN_RUN_LOOP);
        if (err != ZN_OK)
        {
            fprintf(stderr, "[%p] client runloop finished: %s\n",
                    d->tcp, zn_strerror(err));
        }
        d->messageSender.stop ();
    }

} // znet

namespace znet {
    
        static void lineTcpServerOnAccept(void *ud, zn_Accept *accept, unsigned err, zn_Tcp *tcp);
        static void lineTcpServerOnRecv(void *ud, zn_Tcp *tcp, unsigned err, unsigned count);
    
        struct LineTcpServer::Impl
        {
            LineReceivedCallback lineReceivedCb = nullptr;
            ClientConnectedCallback clientConnectedCb = nullptr;
            ClientDisconnectedCallback clientDisconnectedCb = nullptr;

            std::string serverIp;
            int serverPort = -1;
            
            zn_Accept* accept = nullptr;
            zn_State* state = nullptr;
            zn_Tcp* tcp = nullptr;

            std::vector<char> recvBuffer;
            std::string currentLine;
    
            int numConnectionAttempts = 0;
    
            std::atomic_bool listening { false };

            int activeConnectionId = 0;

            TcpLineReader lineReader;
            MessageSender messageSender;

            void onAccept (unsigned err, zn_Tcp* incomingTcp)
            {
                if (err != ZN_OK)
                { /* not lucky? let's try again. */
                    /* we use ud to find out which time we tried. */
                    fprintf(stderr, "[%p] server could not accept client. (%s)\n",
                            tcp, zn_strerror(err));
                    return;
                }

                ++activeConnectionId;
                tcp = incomingTcp;
    
                messageSender.start (tcp);

                recvBuffer.resize (2048);
                zn_recv(tcp, recvBuffer.data(), (unsigned)recvBuffer.size(), lineTcpServerOnRecv, this);
    
                fprintf(stderr, "[%p] server accepted a client!\n", tcp);
                listening = true;
    
                if (clientConnectedCb)
                    clientConnectedCb(activeConnectionId);

                // Note: would need to call accept here to accept several clients.
            }
    
            void onMessageSent(unsigned err, unsigned count)
            {
                /* send work may error out, we first check the result code: */
                if (err != ZN_OK)
                {
                    fprintf(stderr, "[%p] client error when sending something: %s\n",
                            tcp, zn_strerror(err));
                    closeConnection();
                    return;
                }
            }
    
            void onMessageReceived(unsigned err, unsigned count)
            {
                if (err != ZN_OK)
                {
                    fprintf(stderr, "[%p] server error when receiving something: %s\n",
                            tcp, zn_strerror(err));
                    closeConnection();
                    return;
                }
    
                std::cerr << "server received " << count << " bytes" << std::endl;
    
                lineReader.processNewInputData (recvBuffer, count, lineReceivedCb);
    
                zn_recv(tcp, recvBuffer.data(), (unsigned)recvBuffer.size(), lineTcpServerOnRecv, this);
            }

            void closeConnection ()
            {
                messageSender.stop ();
                zn_deltcp(tcp); /* and we close connection. */
                tcp = nullptr;
                if (clientDisconnectedCb)
                    clientDisconnectedCb(activeConnectionId);                
            }
        };
    
        LineTcpServer::LineTcpServer()
        : d (new Impl())
        {}
    
        LineTcpServer::~LineTcpServer () = default;
    
        static void lineTcpServerOnAccept(void *ud, zn_Accept *accept, unsigned err, zn_Tcp *tcp) {
            LineTcpServer::Impl *impl = (LineTcpServer::Impl*)ud;
            impl->onAccept (err, tcp);
        }
    
        static void lineTcpServerOnRecv(void *ud, zn_Tcp *tcp, unsigned err, unsigned count) {
            LineTcpServer::Impl *impl = (LineTcpServer::Impl*)ud;
            impl->onMessageReceived (err, count);
        }
    
        void LineTcpServer::setLineReceivedCallback (LineReceivedCallback cb)
        {
            assert (!d->listening); // Cannot assign it once connected, not thread-safe.
            if (!d->listening)
                d->lineReceivedCb = cb;
        }
    
        void LineTcpServer::setClientConnectedCallback (ClientConnectedCallback cb)
        {
            assert (!d->listening); // Cannot assign it once connected, not thread-safe.        
            if (!d->listening)
                d->clientConnectedCb = cb;
        }

        void LineTcpServer::setClientDisconnectedCallback (ClientDisconnectedCallback cb)
        {
            assert (!d->listening); // Cannot assign it once connected, not thread-safe.        
            if (!d->listening)
                d->clientDisconnectedCb = cb;
        }
    
        bool LineTcpServer::startListening (int port)
        {
            zn_initialize(); // safe to call multiple times.
            d->state = zn_newstate();
            if (!d->state) {
                fprintf(stderr, "[ZNET] create handler failed\n");
                return false;
            }

            /* create a znet tcp server */
            d->accept = zn_newaccept(d->state);

            /* this server listen to 8080 port */
            int err = zn_listen(d->accept, "0.0.0.0", port);
            if (err == ZN_OK)
            {
                fprintf(stderr, "[%p] accept listening to %d ...\n", d->accept, port);
            }
            else
            {
                fprintf (stderr, "[ZNET] Could not listen on port %d (%s)\n", port, zn_strerror(err));
                return false;
            }

            /* this server and when new connection coming, on_accept()
             * function will be called.
             * the 3rd argument of zn_accept will be send to on_accept as-is.
             * we don't use this pointer here, but will use in when send
             * messages. (all functions that required a callback function
             * pointer all have this user-data pointer */
            zn_accept(d->accept, lineTcpServerOnAccept, d.get());
            return true;
        }
    
        bool LineTcpServer::disconnect ()
        {
            zn_close(d->state);
            zn_deinitialize();
            return true;
        }

        bool LineTcpServer::sendString (const std::string& str)
        {
            if (!d->tcp)
            {
                fprintf (stderr, "Connection closed!\n");
                return false;
            }

            return d->messageSender.sendString (str);
        }
        
        void LineTcpServer::runLoop ()
        {
            int err = ZN_OK;
            while (err == ZN_OK)
            {
                err = zn_run(d->state, ZN_RUN_LOOP);
                if (err != ZN_OK)
                {
                    fprintf(stderr, "[%p] client runloop finished: %s\n",
                            d->tcp, zn_strerror(err));
                }

                // Finished the loop, means the client disconnected. Now
                // create a new accept to get ready for a new client.
                zn_accept(d->accept, lineTcpServerOnAccept, d.get());
            }
        }
    
    } // znet

namespace znet {

    class ClockSynchronizer
    {
        struct ClockRequest
        {
            uint64_t msecsClient = 0;
            int requestId = -1;
        };

    public:
        ClockSynchronizer (LineTcpClient* client, LineTcpServer* server)
        : _client (client), _server(server)
        {}

        void clientStartSynchronizing ()
        {
            _requests.resize (_numRequestsToAverage);
            _clockOffsets.reserve (_numRequestsToAverage);
            sendSyncRequest ();
        }

        void sendSyncRequest ()
        {
            std::stringstream ss;
            
            auto nowClientMicrosecs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            ss << "CLOCK " << _nextRequestId << std::endl;

            auto& rq = _requests[_nextRequestId];
            rq.msecsClient = nowClientMicrosecs;
            rq.requestId = _nextRequestId;

            _client->sendString (ss.str());

            ++_nextRequestId;
        }

        void onReceiveLineFromServer (const std::string& line)
        {
            auto nowClientMicrosecs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            
            std::istringstream stream (line);
            std::string command;
            stream >> command;
            if (stream.fail())
                return;
            if (command != "CLOCK")
                return;
            int requestId = -1;
            stream >> requestId;
            if (stream.fail())
                return;

            uint64_t serverNowMsecs = 0;
            stream >> serverNowMsecs;
            if (stream.fail())
                return;

            assert (requestId < _requests.size());
            auto& rq = _requests[requestId];
            const int64_t latencyMsecs = nowClientMicrosecs - rq.msecsClient;
            const int64_t deltaMsecs = serverNowMsecs - rq.msecsClient - (latencyMsecs/2);
            fprintf (stderr, "Offset in usecs = %lld (latency=%lld)\n", deltaMsecs, latencyMsecs);

            _clockOffsets.push_back (deltaMsecs);

            if (_clockOffsets.size() < _numRequestsToAverage)
            {
                sendSyncRequest ();
            }
            else
            {            
                std::sort (_clockOffsets.begin(), _clockOffsets.end()); 
                // take the median value and go to seconds.            
                double offset = _clockOffsets[_clockOffsets.size()/2] * 1e-6;
                _estimatedClientFromServerOffset = offset;

                fprintf (stderr, "Offsets ");
                for (int i = 0; i < _clockOffsets.size(); ++i)
                {
                    fprintf (stderr, "%f ", _clockOffsets[i]*1e-6);
                }
                fprintf (stderr, "\n");
                fprintf (stderr, "Median clock offset = %f\n", offset);
            }
        }

        void onReceiveLineFromClient (const std::string& line)
        {
            std::istringstream stream (line);
            std::string command;
            stream >> command;
            fprintf (stderr, "CLOCK received command '%s'\n", command.c_str());
            if (stream.fail())
                return;
            if (command != "CLOCK")
                return;
            int requestId = -1;
            stream >> requestId;
            if (stream.fail())
                return;

            std::stringstream ss;

            auto nowMicrosecs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            ss << "CLOCK " << requestId << " " << nowMicrosecs << std::endl;
            _server->sendString(ss.str());
        }

    private:
        const int _numRequestsToAverage = 100;
        LineTcpClient* _client = nullptr;
        LineTcpServer* _server = nullptr;
        int _nextRequestId = 0;
        std::vector<ClockRequest> _requests;
        std::vector<double> _clockOffsets;
        std::atomic<double> _estimatedClientFromServerOffset { NAN };
    };
} // znet

int mainClient()
{
    znet::LineTcpClient tcpClient;
    
    std::unique_ptr<znet::ClockSynchronizer> clockSync;

    tcpClient.setLineReceivedCallback ([&](const std::string& str) {
        std::cerr << "Received line: " << str;
        clockSync->onReceiveLineFromServer(str);
    });

    tcpClient.setConnectedToServerCallback ([&]() {
        std::cerr << "Connected!" << std::endl;
        tcpClient.sendString("hello\n");

        clockSync.reset (new znet::ClockSynchronizer(&tcpClient, nullptr));
        clockSync->clientStartSynchronizing ();
    });

    std::thread t ([&tcpClient](){
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            tcpClient.sendString ("proutFromThread\n");
        }
    });

    tcpClient.connectToServer ("127.0.0.1", 4998);
    tcpClient.runLoop ();
    return 0;
}

int mainServer()
{
    znet::LineTcpServer tcpServer;
    
    std::unique_ptr<znet::ClockSynchronizer> clockSync;

    tcpServer.setLineReceivedCallback ([&](const std::string& str) {
        std::cerr << "Received line: " << str;
        clockSync->onReceiveLineFromClient(str);
    });

    std::thread t;
    bool shouldStop = false;

    tcpServer.setClientConnectedCallback ([&](int id) {
        std::cerr << "Client connected!" << std::endl;
        tcpServer.sendString("hello from server\n");
        shouldStop = false;
        clockSync.reset (new znet::ClockSynchronizer(nullptr, &tcpServer));
        t = std::thread ([&](){
            while (!shouldStop)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                tcpServer.sendString ("proutFromThread\n");
            }
        });
    });

    tcpServer.setClientDisconnectedCallback ([&](int id) {
        shouldStop = true;
        t.join ();
    });

    if (!tcpServer.startListening (4998))
    {
        std::cerr << "Fatal error, exiting." << std::endl;
        return 1;
    }
    tcpServer.runLoop ();
    return 0;
}

int main ()
{
    // return mainServer ();
    
    // Test server on macOS
    // socat -v tcp-l:4998,reuseaddr,fork exec:'/bin/cat'
    return mainClient ();
}