#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>

namespace znet
{

    using LineReceivedCallback = std::function<void(const std::string&)>;
    using ConnectionReceivedCallback = std::function<void(int connectionId)>;
    using ClientConnectedCallback = std::function<void(int clientId)>;
    using ClientDisconnectedCallback = std::function<void(int clientId)>;    
    using ConnectedToServerCallback = std::function<void(void)>;

    class LineTcpClient
    {
    public:
        LineTcpClient ();
        ~LineTcpClient ();

        void setLineReceivedCallback (LineReceivedCallback cb);
        void setConnectedToServerCallback (ConnectedToServerCallback cb);

    public:
        bool connectToServer (const std::string& serverIp, int port);
        void runLoop();

        bool disconnect ();    
        bool sendString (const std::string& str);

    public:
        struct Impl; friend struct impl;

    private:
        std::unique_ptr<Impl> d;
    };

    class LineTcpServer
    {
    public:
        LineTcpServer();
        ~LineTcpServer();

    public:
        void setLineReceivedCallback (LineReceivedCallback cb);
        void setClientConnectedCallback (ClientConnectedCallback cb);
        void setClientDisconnectedCallback (ClientDisconnectedCallback cb);

    public:
        bool startListening (int port);
        void runLoop();

        bool disconnect ();
        bool sendString (const std::string& str);

    public:
        struct Impl; friend struct impl;

    private:
        std::unique_ptr<Impl> d;
    };

    class ClockSynchronizer
    {
    private:
        struct ClockRequest
        {
            uint64_t msecsClient = 0;
            int requestId = -1;
        };

    public:
        ClockSynchronizer(LineTcpClient* client, LineTcpServer* server);

        void clientStartSynchronizing();
        void onReceiveLineFromServer(const std::string& line);
        void onReceiveLineFromClient(const std::string& line);

    private:
        void sendSyncRequest();

    private:
        const int _numRequestsToAverage = 100;
        LineTcpClient* _client = nullptr;
        LineTcpServer* _server = nullptr;
        int _nextRequestId = 0;
        std::vector<ClockRequest> _requests;
        std::vector<double> _clockOffsets;
        std::atomic<double> _estimatedClientFromServerOffset{ NAN };
    };

} // znet
