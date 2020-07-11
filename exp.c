#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#define BUFLEN 8192
#define STRLEN 16
#define TSLEN 20
#define FMT "%Y-%m-%d %H:%M:%S"

int main (void)
{
    printf ("Connecting to server...\n");
    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    zmq_connect (requester, "tcp://138.201.156.239:5566");
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char buffer[BUFLEN], string[BUFLEN];
    int size;
    char de[STRLEN], dx[STRLEN], timestring1[TSLEN], 
        timestring2[TSLEN], extradata[STRLEN];
    int x1, x2, x3, x4, snr, speed, got, mode;
    time_t time1, time2;
    float freq;
    struct tm *stime;
    
    while (true)
    {
        size = zmq_recv (requester, buffer, BUFLEN, 0);
        memcpy(string, buffer, size);
        string[size] = 0;       
        
        if (strncmp(buffer, "PROD_SPOT_", 10) == 0) 
        {
            size = zmq_recv (requester, buffer, BUFLEN, 0);
            memcpy(string, buffer, size);
            string[size] = 0;
            //  QRG     call    spotter               spotted         received
            // 7029.00|DL2DXA/W|9A1CIG|1|1|10|20|1|1|1594465469081|1594465469280|rx1

        got = sscanf(string, "%f|%[^|]|%[^|]|%d|%d|%d|%d|%d|%d|%ld|%ld|%s",
            &freq, dx, de, &x1, &x2, &snr, &speed, &x3, &x4, &time1, &time2, extradata);

        stime = gmtime(&time1);
        (void)strftime(timestring1, TSLEN, FMT, stime);

        stime = gmtime(&time2);
        (void)strftime(timestring2, TSLEN, FMT, stime);

        if (got == 12)
            printf("Freq=%7.1f DX=%9s DE=%9s X1=%2d x2=%2d SNR=%2d SP=%2d X3=%2d X4=%2d t1=%s t2=%s EX=%s\n", 
                freq, dx, de, x1, x2, snr, speed, x3, x3, timestring1, timestring2, extradata);
        }
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);

    return 0;
}
