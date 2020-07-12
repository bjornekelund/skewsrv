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
#define BANDNAMES {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m" }
#define BANDS 12
#define CW 1
#define CQ 1
#define DX 2
#define REFFILENAME "reference"
#define USAGE "Usage: %s [-d]\n"
#define ZMQURL "tcp://138.201.156.239:5566"

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
#define MAXINACT 300
// Time constant in qualified spots of filter for error at 14MHz
#define TC 50

#define DEBUG true

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

struct Bandinfo
{
    char name[STRLEN];  // Human friendly name of band
    long count;         // Number of analyzed spots
    bool active;        // Heard from in MAXINACT seconds or less
    double avadj;       // Average deviation as factor
    float avdev;       // Average deviation in ppm
    float absavdev;    // Absolute average deviation in ppm
    time_t last;        // Most recent spot analyzed
};

struct Skimmer
{
    char name[STRLEN];  // Skimmer callsign
    bool reference;     // Is a reference skimmer
    float avdev;       // Average deviation across active bands
    struct Bandinfo band[BANDS];
};

static struct Spot pipeline[SPOTSWINDOW];
static struct Skimmer skimmer[MAXSKIMMERS];
static char referenceskimmer[MAXREF][STRLEN];
static int Skimmers = 0, Referenceskimmers = 0;
static long long int Totalspots = 0, Qualifiedspots = 0;

void printstatus(char *string, int line)
{
    printf("\033[%d;H", 20 + line);
    printf("%s", string);
    for (int i = strlen(string); i < 80; i++)
        printf(" ");
}

void printstatuscall(char *call1, char *call2, char *call3, char *call4, int line)
{
    char call[4][STRLEN];
    
    strcpy(call[0], call1);
    strcpy(call[1], call2);
    strcpy(call[2], call3);
    strcpy(call[3], call4);
    
    int col = 1;
    for (int cn = 0; cn < 4; cn++)
    {
        for (int skimpos = 0; skimpos < Skimmers; skimpos++)
        {
            if (strcmp(call[cn], skimmer[skimpos].name) == 0)
            {
                int j  = line;
                for (int band = 0; band < BANDS; band++)
                {
                    if (skimmer[skimpos].band[band].active)
                    {
                        printf("\033[%d;%dH", 23 + j++, col);
                        printf("%s: %s %+6.2fppm", skimmer[skimpos].name, 
                            skimmer[skimpos].band[band].name, skimmer[skimpos].band[band].avdev); 
                    }
                }
                for (int k = j; k < 8; k++)
                {
                    printf("\033[%d;%dH                       ", 24 + j++, col);
                }
                col += 25;
            }
        }
    }
}

int fqbandindex(double freq)
{
    switch ((int)round(freq / 1000.0))
    {
        case 2: // 160m
            return 0;
        case 3:
        case 4: // 80m
            return 1;
        case 5: // 60m
            return 2;
        case 7: // 40m
            return 3;
        case 10: // 30m
            return 4;
        case 14: // 20m
            return 5;
        case 18: // 17m
            return 6;
        case 21: // 15m
            return 7;
        case 25: // 12m
            return 8;
        case 28: // 10m
        case 29:
        case 30:
            return 9;
        case 50: // 6m
        case 51:
        case 52:
        case 53:
        case 54:
            return 10;
        case 144: // 2m
        case 145:
        case 146:
            return 11;
        default:
            return -1;
    }
} 

int main(int argc, char *argv[])
{
    const char *bandname[] = BANDNAMES;
    char buffer[BUFLEN], tmpstring[BUFLEN];
    int c, spp = 0;
    FILE *fr;
    bool debug = DEBUG;
    long long int lastspotcount = 0; 
    float spotsperminute;
    
    void *context = zmq_ctx_new();
    void *requester = zmq_socket(context, ZMQ_SUB);
    zmq_connect(requester, ZMQURL);
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    

    while ((c = getopt(argc, argv, "d")) != -1)
    {
        switch (c)
        {
            case 'd': 
                debug = true;
                break;
            case '?':
                fprintf(stderr, USAGE, argv[0]);
                return 1;
            default:
                abort();
        }
    }

    if (debug)
        printf("Connecting to server...\n");

    // Avoid that unitialized entries in pipeline are used
    for (int i = 0; i < SPOTSWINDOW; i++)
        pipeline[i].analyzed = true;
    
    for (int i = 0; i < MAXSKIMMERS; i++)
    {
        skimmer[i].name[0]  = 0;
        for (int j = 0; j < BANDS; j++)
        {
            skimmer[i].band[j].active = false;
            strcpy(skimmer[i].band[j].name, bandname[j]);            
        }
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
                strcpy(referenceskimmer[Referenceskimmers], tempstring);
                Referenceskimmers++;
                if (Referenceskimmers >= MAXREF) 
                {
                    fprintf(stderr, "Overflow: Last reference skimmer read is %s.\n", tempstring);
                    (void)fclose(fr);
                    return 1;
                }
            }
        }
    }

    (void)fclose(fr);

    if (debug)
    {
        // Clear screen of console
        printf("\033c");
    }

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
                
                if (debug)
                {
                    struct tm *stime;
                    stime = gmtime(&spottime);
                    (void)strftime(timestring, TSLEN, "%Y-%m-%d %H:%M:%S", stime);

                    // printf("%s\n", string);
                    // printf("got=%2d Freq=%7.1f DX=%9s DE=%9s type=%2d bf=%2.1f SNR=%2d SP=%2d md=%2d ntp=%1d t=%s EX=%s\n", 
                        // got, freq, dx, de, spot_type, base_freq, snr, speed, mode, ntp, timestring, extradata);
                }
                
                Totalspots++;
                
                // If SNR is sufficient and frequency OK and mode is right
                if (snr >= MINSNR && freq >= MINFREQ && mode == CW && (spot_type == CQ || spot_type == DX))
                {
                    // Check if this spot is from a reference skimmer
                    bool reference = false;
                    for (int i = 0; i < Referenceskimmers; i++)
                    {
                        if (strcmp(de, referenceskimmer[i]) == 0)
                        {
                            reference = true;
                            break;
                        }
                    }

                    // If it is reference spot, use it to check all un-analyzed 
                    // spots of the same call and in the pipeline
                    if (reference)
                    {                    
                        printstatuscall("F6IIT", "OK2EW", "SM6FMB", "SM0IHR", 0);
                        
                        for (int i = 0; i < SPOTSWINDOW; i++)
                        {
                            // delta is frequency deviation in kHz
                            double deltakhz = pipeline[i].freq - freq;
                            double deltappm = 1000000.0 * deltakhz / freq;
                            double adeltakhz = fabs(deltakhz);
                            double adeltappm = fabs(deltappm);
                            
                            int bandindex = fqbandindex(freq);
                            
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
                                
                                Qualifiedspots++;
                                
                                pipeline[i].analyzed = true; // To only analyze each spot once

                                // Check if this skimmer is already in list
                                int skimpos = -1;
                                for (int j = 0; j < Skimmers; j++)
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
                                    const double tc = TC;
                                    const double basefq = 14000.0;
                                    double factor = freq / (tc * basefq);
                                    skimmer[skimpos].band[bandindex].avadj = 
                                        (1.0 - factor) * skimmer[skimpos].band[bandindex].avadj + 
                                        factor * pipeline[i].freq / freq;

                                    skimmer[skimpos].band[bandindex].avdev = 
                                        (1.0 - factor) * skimmer[skimpos].band[bandindex].avdev + factor * deltappm;
                                        
                                    skimmer[skimpos].band[bandindex].count++;
                                    skimmer[skimpos].band[bandindex].last = pipeline[i].time;
                                    if (!skimmer[skimpos].band[bandindex].active && debug)
                                    {
                                        sprintf(tmpstring, "Skimmer %s marked active on %s", skimmer[skimpos].name, 
                                            skimmer[skimpos].band[bandindex].name);
                                        printstatus(tmpstring, 1);
                                        sprintf(tmpstring, "Total spots: %lld with %.1f spots per minute and %.1f%% qualified spots", 
                                            Totalspots, spotsperminute, 100.0 * Qualifiedspots / (float)Totalspots);
                                        printstatus(tmpstring, 2);
                                    }

                                    skimmer[skimpos].band[bandindex].active = true;
                                    // if (debug)
                                        // printf("Skimmer %-9s Dev %+6.2f\n", 
                                            // strcat(skimmer[skimpos].name, skimmer[skimpos].reference ? "*" : ""),
                                            // 1000000.0 * (skimmer[skimpos].avadj - 1.0));
                                    if (debug && skimpos < 96)
                                    {
                                        // printf("Skimmer %-9s Dev %+5.2f %s\n", 
                                            // skimmer[skimpos].name, 1000000.0 * (skimmer[skimpos].avadj - 1.0),
                                            // skimmer[skimpos].reference ? "+" : "");
                                        printf("\033[H");
                                        for (int i = 0; i < 95; i++)
                                        {
                                            double averr = 0.0;
                                            int active = 0;
                                            for (int j = 0; j < BANDS; j++)
                                            {
                                                if (skimmer[i].band[j].active)
                                                {
                                                    averr += skimmer[i].band[j].avdev;
                                                    active++;
                                                }
                                            }
                                            if (active != 0)
                                                printf("%10s:%+6.2lf(%d)", skimmer[i].name, averr / active, active);
                                            else 
                                            {
                                                if (i < Skimmers)
                                                    printf("%10s:%9s", skimmer[i].name, "");
                                                else
                                                    printf("%10s %9s", "", "");
                                            }
                                                
                                            if ((i + 1) % 5 == 0)
                                                printf("\n");                                                
                                        }
                                        printf("\n");
                                    }
                                }
                                else // If new skimmer, add it to list
                                {
                                    if (Skimmers >= MAXSKIMMERS) 
                                    {
                                        fprintf(stderr, "Skimmer list overflow (%d). Clearing list.\n", Skimmers);
                                        Skimmers = 0;                                        
                                    }
                                    strcpy(skimmer[Skimmers].name, pipeline[i].de);
                                    skimmer[Skimmers].band[bandindex].avadj = 1.0; // Guess zero error as start
                                    skimmer[Skimmers].band[bandindex].avdev = 0.0; // Guess zero error as start
                                    skimmer[Skimmers].band[bandindex].count = 1;
                                    skimmer[Skimmers].band[bandindex].last = pipeline[i].time;
                                    skimmer[Skimmers].reference = pipeline[i].reference;
                                    skimmer[Skimmers].band[bandindex].active = true;

                                    Skimmers++;
                                    // if (debug)
                                        // fprintf(stderr, "Found skimmer #%d: %s \n", Skimmers, pipeline[i].de);
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

                    // Move pipeline pointer and wrap around at top of pipeline
                    spp = (spp + 1) % SPOTSWINDOW;
                }
            }
            else
            {
                if (debug)
                    printf("Failed parsing of spot!\n");
            }
        }
        // Check for inactive skimmers once per minute
        time_t nowtime;
        time(&nowtime);
        if (nowtime % 15 == 0) // Four times per minute
        {
            // Estimate spots per minute. Filter with tc=10 15 second periods.
            long long int periodcount = Totalspots - lastspotcount;
            spotsperminute = (9.0 * spotsperminute + 4.0 * periodcount) / 10.0;
            lastspotcount = Totalspots;
            
            // Walk through all identified skimmers for all bands
            for (int i = 0; i < Skimmers; i++)
            {
                for (int j = 0; j < BANDS; j++)
                {
                    if (difftime(nowtime, skimmer[i].band[j].last) >= MAXINACT)
                    {
                        skimmer[i].band[j].active = false;
                        if (skimmer[i].band[j].active && debug)
                        {
                            sprintf(tmpstring, "Skimmer %s marked inactive on %s - no spots for %.0f seconds", 
                                skimmer[i].name, skimmer[i].band[j].name,
                                difftime(nowtime, skimmer[i].band[j].last));
                            printstatus(tmpstring, 0);
                        }
                    }
                }
            }
        }
    }
    
    zmq_close(requester);
    zmq_ctx_destroy(context);

    return 0;
}
