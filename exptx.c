#define _XOPEN_SOURCE
#include "zmq.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define BUFLEN 256
#define STRLEN 16
//#define ZMQTALKURL "tcp://138.201.156.239:5567"
#define ZMQTALKURL "tcp://localhost:80"


int main(int argc, char *argv[])
{
    char pbuffer[BUFLEN];

    void *context = zmq_ctx_new();
    void *publisher = zmq_socket(context, ZMQ_PUB);
    int trc = zmq_connect(publisher, ZMQTALKURL);

    printf("Established talk context and socket with %s status\n", trc == 0 ? "OK" : "NOT OK");

    while (!false) // Replace false with close down signal
    {
        strcpy(pbuffer, "SKEW_TEST");

        printf("Sent \"%s\"\n", pbuffer);

        int size = zmq_send(publisher, pbuffer, strlen(pbuffer), 0);
        assert(size == strlen(pbuffer));
        sleep(1);
    }

    return 0;
}
