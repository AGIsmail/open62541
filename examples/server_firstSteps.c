//This file contains source-code that is discussed in a tutorial located here:
// http://open62541.org/doc/sphinx/tutorial_firstStepsServer.html

#include <stdio.h>
#include <signal.h>

#ifdef UA_NO_AMALGAMATION
# include "ua_types.h"
# include "ua_server.h"
# include "logger_stdout.h"
# include "networklayer_tcp.h"
#else
# include "open62541.h"
#endif

UA_Boolean running = true;
UA_Logger logger = Logger_Stdout;

static void stopHandler(int signal) {
    running = false;
}

int main(void) {
    signal(SIGINT,  stopHandler);
    signal(SIGTERM, stopHandler);

    UA_Server *server = UA_Server_new(UA_ServerConfig_standard);
    UA_Server_setLogger(server, logger);
    UA_Server_addNetworkLayer(server, ServerNetworkLayerTCP_new(UA_ConnectionConfig_standard, 16664));
    UA_Server_run(server, 1, &running);
    UA_Server_delete(server);

    printf("Terminated\n");
    return 0;
}
