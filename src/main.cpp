#include "matching_engine.h"
#include "tcp_server.h"
#include <csignal>

int main()
{
    signal(SIGPIPE, SIG_IGN);

    MatchingEngine engine;

    TcpServer server(
        2250,
        engine
    );

    server.start();

    return 0;
}
