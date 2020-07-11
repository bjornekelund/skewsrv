//  Hello World client
// #include <zmq.h>
// #include <string.h>
// #include <stdio.h>
// #include <unistd.h>

#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define BUFLEN 8192

// int main(void) {
    // void *context = zmq_init(1);
    // void *socket = zmq_socket(context, ZMQ_SUB);
    // zmq_connect(socket, "tcp://138.201.156.239:5566");
    // zmq_setsockopt(socket, ZMQ_SUBSCRIBE, "", 0);
    // int status;

    // char string[BUFLEN] = "";
    // printf ("Connecting to server...\n");

    // while(true) {
        // zmq_msg_t in_msg;
        // // zmq_msg_init(&in_msg);
        // status = zmq_recv(socket, &in_msg, BUFLEN, 0);
        // int size = zmq_msg_size (&in_msg);
        // memcpy(string, zmq_msg_data(&in_msg), size);
        // // zmq_msg_close(&in_msg);
        // string[size] = 0;
        // printf("s=%d m=%s\n", status, string);

        // // sleep(1);
  // }
// }
int main (void)
{
    printf ("Connecting to server...\n");
    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    zmq_connect (requester, "tcp://138.201.156.239:5565");
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char buffer[BUFLEN], string[BUFLEN];
    int size;

    while (true)
    {
        size = zmq_recv (requester, buffer, BUFLEN, 0);
        memcpy(string, buffer, size);
        string[size] = 0;
        // if (strncmp(buffer, "PROD_SPOT_", 10) == 0) 
        // {
            // size = zmq_recv (requester, buffer, BUFLEN, 0);
            // memcpy(string, buffer, size);
            // string[size] = 0;
            printf("SPOT: %s\n", string);
        // }
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);

    return 0;
}
