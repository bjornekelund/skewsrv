//  Hello World client
#include <zmq.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define BUFLEN 8192
int main (void)
{
    printf ("Connecting to server...\n");
    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    int status;
    zmq_connect (requester, "tcp://138.201.156.239:5565");
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "",0);
    int request_nbr;
    char buffer [BUFLEN];

    for (request_nbr = 0; request_nbr != 7; request_nbr++) {
        status = zmq_recv (requester, buffer, BUFLEN, 0);
	printf("STAT=%d, MSG=\"%s\"\n\n", status, buffer);
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);
    return 0;
}
