#include <string>
#include <functional>

namespace znet
{

    using LineReceivedCallback = std::function<void(const std::string&)>;
    using ConnectionReceivedCallback = std::function<void(int connectionId)>;
    using ClientConnectedCallback = std::function<void(void)>;

    class LineTcpClient
    {
    public:
        LineTcpClient ();
        ~LineTcpClient ();

        void setLineReceivedCallback (LineReceivedCallback cb);
        void setClientConnectedCallback (ClientConnectedCallback cb);

    public:
        bool connectToServer (const std::string& serverIp, int port);
        void waitUntilConnected ();
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
        void setLineReceivedCallback (LineReceivedCallback cb);

    public:
        bool startListening (int port);
        bool disconnect ();
        bool sendString (const std::string& str);

    public:
        struct Impl; friend struct impl;

    private:
        std::unique_ptr<Impl> d;
    };

} // znet
