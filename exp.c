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
    char de[STRLEN], dx[STRLEN], timestring[TSLEN], 
        extradata[STRLEN];
    int snr, speed, got;
    time_t jstime1, jstime2, spottime;
    float freq;
    struct tm *stime;
    // char field[12][STRLEN];
    int spot_type, mode, ntp;
    float base_freq;
    
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

            got = sscanf(string, "%f|%[^|]|%[^|]|%d|%f|%d|%d|%d|%d|%ld|%ld|%s",
                &freq, dx, de, &spot_type, &base_freq, &snr, &speed, &mode, &ntp, &jstime1, &jstime2, extradata);

            spottime = jstime2 / 1000;
            
            stime = gmtime(&spottime);
            (void)strftime(timestring, TSLEN, FMT, stime);

            // printf("%s\n", string);
            printf("got=%2d Freq=%7.1f DX=%9s DE=%9s type=%2d bf=%2.1f SNR=%2d SP=%2d md=%2d ntp=%1d t=%s EX=%s\n", 
                got, freq, dx, de, spot_type, base_freq, snr, speed, mode, ntp, timestring, extradata);
        }
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);

    return 0;
}
