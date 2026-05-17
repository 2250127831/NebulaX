#include "matching_engine.h"
#include "tcp_server.h"

int main()
{
    MatchingEngine engine;

    TcpServer server(
        2250,
        engine
    );

    server.start();

    return 0;
}
