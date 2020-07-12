#define _XOPEN_SOURCE
#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#define BUFLEN 256
#define STRLEN 16
#define TSLEN 20
#define CW 1
#define CQ 1
#define DX 2
#define REFFILENAME "reference"

// Maximum number of skimmers. Overflow is handled gracefully.
#define MAXSKIMMERS 500
// Maximum number of reference spots. Overflow stops reading list.
#define MAXREF 50
// Window of recent spots
#define SPOTSWINDOW 1000

// Maximum time to reference spot for spot to qualify
#define MAXAPART 30
// Maximum error in kHz and PPM for spot to qualify
// 0.5kHz is 18ppm on 10m
#define MAXERRKHZ 0.5
// 60ppm is 0.11kHz on 160m
#define MAXERRPPM 60
// Maximum SNR for spot to qualify
#define MINSNR 6
// Minimum frequency for spot to qualify
#define MINFREQ 1800.0
// Maximum seconds since last spot to be considered active
#define MAXSILENCE 180

#define DEBUG true

int main(void)
{
    struct Spot 
    {
        char de[STRLEN];   // Skimmer callsign
        char dx[STRLEN];   // Spotted call
        time_t time;       // Spot timestamp in epoch format
        int snr;           // SNR for spotcount
        double freq;       // Spot frequency
        bool reference;    // Originates from a reference skimmer
        bool analyzed;     // Already analyzed
    };

    struct Skimmer
    {
        char name[STRLEN];  // Skimmer callsign
        double avadj;       // Average deviation as factor
        double avdev;       // Average deviation in ppm
        double absavdev;    // Absolute average deviation in ppm
        int count;          // Number of analyzed spots
        time_t last;        // Most recent spot analyzed
        bool reference;     // Is a reference skimmer
        bool inactive;       // If not heard from for MAXSILENCE seconds
    };

    void printstatus(char *string, int line)
    {
        if (DEBUG)
        {
            printf("\033[%d;H", 18 + line);
            printf("%s", string);
            for (int i = strlen(string); i < 80; i++)
                printf(" ");
        }
    }

    printf("Connecting to server...\n");
    void *context = zmq_ctx_new();
    void *requester = zmq_socket(context, ZMQ_SUB);
    zmq_connect(requester, "tcp://138.201.156.239:5566");
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char buffer[BUFLEN], tmpstring[BUFLEN];
    char referenceskimmer[MAXREF][STRLEN];
    int spp = 0, skimmers = 0, referenceskimmers = 0;
    long long int totalspots = 0; // , usedspots = 0;
    FILE *fr;
    
    struct Spot pipeline[SPOTSWINDOW];
    struct Skimmer skimmer[MAXSKIMMERS];

    // Avoid that unitialized entries in pipeline are used
    for (int i = 0; i < SPOTSWINDOW; i++)
        pipeline[i].analyzed = true;
    
    if (DEBUG)
    {
        for (int i = 0; i < MAXSKIMMERS; i++)
        {
            skimmer[i].avadj = 1.0;
            skimmer[i].name[0]  = 0;
        }
        printf("\033c");
    }

    fr = fopen(REFFILENAME, "r");

    if (fr == NULL) 
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", REFFILENAME);
        return 1;
    }

    while (fgets(buffer, BUFLEN, fr) != NULL)
    {
        char tempstring[BUFLEN];
        if (sscanf(buffer, "%s", tempstring) == 1)
        {
            // Don't include comment lines
            if (tempstring[0] != '#')
            {
                strcpy(referenceskimmer[referenceskimmers], tempstring);
                referenceskimmers++;
                if (referenceskimmers >= MAXREF) 
                {
                    fprintf(stderr, "Overflow: Last reference skimmer read is %s.\n", tempstring);
                    (void)fclose(fr);
                    return 1;
                }

            }
        }
    }

    (void)fclose(fr);

    while (true) // Replace with close down signal
    {
        int size = zmq_recv(requester, buffer, BUFLEN, 0);
        buffer[size] = 0;
        
        if (strncmp(buffer, "PROD_SPOT", 9) == 0) 
        {
            size = zmq_recv(requester, buffer, BUFLEN, 0);
            buffer[size] = 0;
            //  QRG     call    spotter               spotted         received
            // 7029.00|DL2DXA/W|9A1CIG|1|1|10|20|1|1|1594465469081|1594465469280|rx1

            char de[STRLEN], dx[STRLEN], timestring[TSLEN], extradata[STRLEN];
            int snr, speed, spot_type, mode, ntp;
            time_t jstime1, jstime2, spottime;
            double freq, base_freq;

            int got = sscanf(buffer, "%lf|%[^|]|%[^|]|%d|%lf|%d|%d|%d|%d|%ld|%ld|%s",
                &freq, dx, de, &spot_type, &base_freq, &snr, &speed, &mode, &ntp, &jstime1, &jstime2, extradata);

            if (got == 12) 
            {
                spottime = jstime2 / 1000;
                
                if (DEBUG)
                {
                    struct tm *stime;
                    stime = gmtime(&spottime);
                    (void)strftime(timestring, TSLEN, "%Y-%m-%d %H:%M:%S", stime);

                    // printf("%s\n", string);
                    // printf("got=%2d Freq=%7.1f DX=%9s DE=%9s type=%2d bf=%2.1f SNR=%2d SP=%2d md=%2d ntp=%1d t=%s EX=%s\n", 
                        // got, freq, dx, de, spot_type, base_freq, snr, speed, mode, ntp, timestring, extradata);
                }
                
                totalspots++;
                
                // If SNR is sufficient and frequency OK and mode is right
                if (snr >= MINSNR && freq >= MINFREQ && mode == CW && (spot_type == CQ || spot_type == DX))
                {
                    // Check if this spot is from a reference skimmer
                    bool reference = false;
                    for (int i = 0; i < referenceskimmers; i++)
                    {
                        if (strcmp(de, referenceskimmer[i]) == 0)
                        {
                            reference = true;
                            break;
                        }
                    }

                    // If it is reference spot, use it to check all un-analyzed spots of the same call and 
                    // in the pipeline
                    if (reference)
                    {                    
                        for (int i = 0; i < SPOTSWINDOW; i++)
                        {
                            // delta is frequency deviation in kHz
                            double deltakhz = pipeline[i].freq - freq;
                            double deltappm = 1000000.0 * deltakhz / freq;
                            double adeltakhz = fabs(deltakhz);
                            double adeltappm = fabs(deltappm);
                            
                            // If pipeline entry is...
                            if (!pipeline[i].analyzed &&            // not yet analyzed
                                strcmp(pipeline[i].dx, dx) == 0 &&  // has same call as reference spot
                                adeltakhz <= MAXERRKHZ &&           // has low enough absolute deviation
                                adeltappm < MAXERRPPM &&            // has low enough relative deviation
                                strcmp(pipeline[i].de, de) != 0 &&  // is not from same reference skimmer
                                abs((int)difftime(pipeline[i].time, spottime)) <= MAXAPART) // is close enough in time
                            {
                                // ..then we have a qualified spot in pipeline[i]
                                // The data of the reference spot is in freq and de.
                                
                                pipeline[i].analyzed = true; // To only analyze each spot once

                                // Check if this skimmer is already in list
                                int skimpos = -1;
                                for (int j = 0; j < skimmers; j++)
                                {
                                    if (strcmp(pipeline[i].de, skimmer[j].name) == 0)
                                    {
                                        skimpos = j;
                                        break;
                                    }
                                }

                                if (skimpos != -1) // if in the list, update it
                                {
                                    // First order IIR filtering of deviation
                                    // Time constant inversely proportional to 
                                    // frequency normalized at 14MHz
                                    const double tc = 50.0;
                                    const double basefq = 14000.0;
                                    double factor = freq / (tc * basefq);
                                    skimmer[skimpos].avadj = 
                                        (1.0 - factor) * skimmer[skimpos].avadj + 
                                        factor * pipeline[i].freq / freq;

                                    skimmer[skimpos].avdev = 
                                        (1.0 - factor) * skimmer[skimpos].avdev + factor * deltappm;

                                    // if (fabs(newdev) > 0.5)
                                    // {
                                        // printf("\033[18;H");
                                        // printf("                                                                                                  ");
                                        // printf("\033[18;H");
                                        // printf("skimmer=%s newdev=%.2f skimmer[%d].avdev=%.2f pipeline[%d].freq=%.1f freq=%.1f\n", 
                                        // skimmer[skimpos].name, newdev, skimpos, skimmer[skimpos].avdev, i, pipeline[i].freq, freq);
                                    // }
                                        
                                    skimmer[skimpos].count++;
                                    skimmer[skimpos].last = pipeline[i].time;
                                    if (skimmer[skimpos].inactive == true)
                                    {
                                        sprintf(tmpstring, "Skimmer %s marked not silent", skimmer[skimpos].name);
                                        printstatus(tmpstring, 1);
                                    }

                                    skimmer[skimpos].inactive = false;
                                    // if (DEBUG)
                                        // printf("Skimmer %-9s Dev %+6.2f\n", 
                                            // strcat(skimmer[skimpos].name, skimmer[skimpos].reference ? "*" : ""),
                                            // 1000000.0 * (skimmer[skimpos].avadj - 1.0));
                                    if (DEBUG && skimpos < 96)
                                    {
                                        // printf("Skimmer %-9s Dev %+5.2f %s\n", 
                                            // skimmer[skimpos].name, 1000000.0 * (skimmer[skimpos].avadj - 1.0),
                                            // skimmer[skimpos].reference ? "+" : "");
                                        printf("\033[H");
                                        for (int i = 0; i < 96; i++)
                                        {
                                            printf("%10s:%+6.2f ", 
                                                skimmer[i].name, skimmer[i].avdev);
                                                if ((i + 1) % 6 == 0)
                                                    printf("\n");
                                                
                                        }
                                        printf("\n");
                                    }
                                }
                                else // If new skimmer, add it to list
                                {
                                    if (skimmers >= MAXSKIMMERS) 
                                    {
                                        fprintf(stderr, "Skimmer list overflow (%d). Clearing list.\n", skimmers);
                                        skimmers = 0;                                        
                                    }
                                    // if (strcmp(pipeline[i].de, "SM7IUN") == 0)
                                    // {
                                        // printf("Creating new SM7IUN skimmer #%d. pipeline[%d].de=%s\n", skimmers + 1, i, pipeline[i].de);
                                    // }

                                    strcpy(skimmer[skimmers].name, pipeline[i].de);
                                    skimmer[skimmers].avadj = 1.0; // Guess zero error as start
                                    skimmer[skimmers].avdev = 0.0; // Guess zero error as start
                                    skimmer[skimmers].count = 1;
                                    skimmer[skimmers].last = pipeline[i].time;
                                    skimmer[skimmers].reference = pipeline[i].reference;
                                    skimmer[skimpos].inactive = false;

                                    skimmers++;
                                    // if (DEBUG)
                                        // fprintf(stderr, "Found skimmer #%d: %s \n", skimmers, pipeline[i].de);
                                }
                            }
                        }
                    }

                    // Save new spot in pipeline
                    strcpy(pipeline[spp].de, de);
                    strcpy(pipeline[spp].dx, dx);
                    pipeline[spp].freq = freq;
                    pipeline[spp].snr = snr;
                    pipeline[spp].reference = reference;
                    pipeline[spp].analyzed = false;
                    pipeline[spp].time = spottime;

                    // Move pointer and wrap around at top of pipeline
                    spp = (spp + 1) % SPOTSWINDOW;
                }
            }
        }
        // Check for silent skimmers once per minute
        time_t nowtime;
        time(&nowtime);
        // if (nowtime % 60 == MAXSILENCE) // Once per minute
        {
            for (int i = 0; i < skimmers; i++)
                if (difftime(nowtime, skimmer[i].last) >= MAXSILENCE)
                {
                    if (!skimmer[i].inactive)
                    {
                        sprintf(tmpstring, "Skimmer %s marked silent - no spots for %.0f", 
                            skimmer[i].name, difftime(nowtime, skimmer[i].last));
                        printstatus(tmpstring, 0);
                    }
                    skimmer[i].inactive = true;
                }
        }
    }
    
    zmq_close(requester);
    zmq_ctx_destroy(context);

    return 0;
}
