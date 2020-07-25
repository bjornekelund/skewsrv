//  Hello World client
// #include <stdio.h>
// #include <unistd.h>
#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define BUFLEN 8192

// int main(void) {
    // void *context = zmq_init(1);
    // void *socket = zmq_socket(context, ZMQ_SUB);
    // zmq_connect(socket, "tcp://138.201.156.239:5567");
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
    char buffer[BUFLEN];
    char string[BUFLEN];
    int size1;
    int size2;
    int64_t more = 0;
    size_t more_size = sizeof(more);

    printf ("Connecting to server...\n");
    void *context = zmq_ctx_new ();
    void *subscriber = zmq_socket(context, ZMQ_SUB);
    // int rc = zmq_connect (subscriber, "tcp://138.201.156.239:5566");

    int rc = zmq_connect (subscriber, "tcp://localhost:5555");
    // (void)zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "PROD_SPOT", 9);
    assert(rc == 0);
    
    rc = zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "SKEW_TEST", 9);
    assert(rc == 0);

    while (true)
    {
        size1 = zmq_recv (subscriber, buffer, BUFLEN, 0);
        assert(size1 != -1);
        // buffer[size1] = 0;
        // printf("%s\n", buffer);
        // memcpy(string, buffer, size1);

        zmq_getsockopt(subscriber, ZMQ_RCVMORE, &more, &more_size);

        if (more != 0)
        {
            size2 = zmq_recv(subscriber, buffer, BUFLEN, 0);
            memcpy(string, buffer, size2);
        }
        string[size2] = 0;
        // if (strncmp(buffer, "PROD_SPOT_", 10) == 0) 
        // {
            // size = zmq_recv (subscriber, buffer, BUFLEN, 0);
            // memcpy(string, buffer, size);
            // string[size] = 0;
        // printf("Msg: %s\n", string);
        // }
        printf("%s\n", string);
    }
    zmq_close (subscriber);
    zmq_ctx_destroy (context);

    return 0;
}
