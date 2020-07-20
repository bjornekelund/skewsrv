#define _XOPEN_SOURCE
#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <limits.h>

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
// Number of spots between status report to stdout
#define REPORTPERIOD 5000

// Maximum time to reference spot for spot to qualify
#define MAXAPART 60
// Maximum error in kHz and PPM for spot to qualify
// 0.5kHz is 18ppm on 10m
#define MAXERRKHZ 0.5
// 60ppm is 0.11kHz on 160m
#define MAXERRPPM 60.0
// Maximum SNR for spot to qualify
#define MINSNR 6
// Minimum frequency for spot to qualify
#define MINFREQ 1800.0
// Maximum seconds since last spot to be considered active
#define MAXINACT 15 * 60
// Time constant in qualified spots of filter for error at 14MHz
#define TC 50
// Hour UTC to update list of reference skimmers every day
#define REFUPDHOUR 0
// Minute to update list of reference skimmers every day
#define REFUPDMINUTE 30

struct Spot 
{
    char de[STRLEN];    // Skimmer callsign
    char dx[STRLEN];    // Spotted call
    time_t time;        // Spot timestamp in epoch format
    int snr;            // SNR for spotcount
    double freq;        // Spot frequency
    bool reference;     // Originates from a reference skimmer
    bool analyzed;      // Already analyzed
};

struct Bandinfo
{
    // char name[STRLEN];  // Human friendly name of band
    unsigned long int count;     // Number of analyzed spots for this band
    bool active;        // Heard from in MAXINACT seconds or less on this band
    // double avadj;       // Average deviation as factor for this band
    double avdev;       // Average deviation in ppm for this band
    double absavdev;    // Absolute average deviation in ppm for this band
    time_t last;        // Time of most recent qualified spot for this band in epoch
};

struct Skimmer
{
    char call[STRLEN];  // Skimmer callsign
    bool reference;     // Is a reference skimmer
    double avdev;       // Average deviation across active bands, 40m and up
    bool active;        // Heard from in MAXINACT seconds or less
    time_t last;        // Time of most recent qualified spot in epoch
    struct Bandinfo band[BANDS];
};

// Pipeline of incoming spots. Index spp wraps around.
static struct Spot pipeline[SPOTSWINDOW];

// Database of skimmers. Dimensioned to remember all skimmers
// ever visible on RBN but handles overflow gracefully.
static int Skimmers = 0;
static struct Skimmer skimmer[MAXSKIMMERS];

// List of callsigns of reference skimmers
static int Referenceskimmers = 0;
static char referenceskimmer[MAXREF][STRLEN];

// Spot counters. Large enough to last forever.
static unsigned long int Totalspots = 0, Qualifiedspots = 0;

// Human friendly names of bands
const char *bandname[] = BANDNAMES;

void printstatus(char *string, int line)
{
    printf("\033[%d;H", 20 + line);
    printf("%s", string);
    for (int i = strlen(string); i < 40; i++)
        printf(" ");
}


// Display deviations for active bands for four callsigns
// at the bottom of the terminal screen
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
            if (strcmp(call[cn], skimmer[skimpos].call) == 0)
            {
                int j  = line;
                for (int band = 0; band < BANDS; band++)
                {
                    if (skimmer[skimpos].band[band].active)
                    {
                        printf("\033[%d;%dH", 20 + j, col);
                        printf("%s:%3s%+6.2fppm(%ld)  ", skimmer[skimpos].call,
                            bandname[band], skimmer[skimpos].band[band].avdev, skimmer[skimpos].band[band].count);
                        j++;
                    }
                }
                for (int k = j; k < 8; k++)
                {
                    printf("\033[%d;%dH", 20 + k, col);
                    printf("                        ");
                }
                col += 29;
            }
        }
    }
}

// Convert a frequeny in kHz into an index 0-11 for 160-2m.
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

void updatereferences()
{
    FILE *fr;
    char line[BUFLEN], tempstring[BUFLEN];

    fr = fopen(REFFILENAME, "r");
    Referenceskimmers = 0;

    if (fr == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", REFFILENAME);
        abort();
    }

    while (fgets(line, BUFLEN, fr) != NULL)
    {
        if (sscanf(line, "%s", tempstring) == 1)
        {
            // Don't include comment lines
            if (tempstring[0] != '#')
            {
                strcpy(referenceskimmer[Referenceskimmers], tempstring);
                Referenceskimmers++;
                if (Referenceskimmers >= MAXREF)
                {
                    fprintf(stderr, "Overflow: Last reference skimmer read is %s.\n", tempstring);
                    break;
                }
            }
        }
    }

    (void)fclose(fr);
}

int main(int argc, char *argv[])
{
    char buffer[BUFLEN], tmpstring[BUFLEN];
    int c, spp = 0, lastday = 0;
    time_t lasttime = 0, nowtime;
    bool debug = false, connected = false;
    unsigned long int lastspotcount = 0;
    double spotsperminute = 0.0;
    char zmqurl[BUFLEN] = ZMQURL;

    while ((c = getopt(argc, argv, "du:")) != -1)
    {
        switch (c)
        {
            case 'd':
                debug = true;
                break;
            case 'u':
                strcpy(zmqurl, optarg);
                break;
            case '?':
                fprintf(stderr, USAGE, argv[0]);
                return 1;
            default:
                abort();
        }
    }

    updatereferences();

    time(&nowtime);
	lasttime = nowtime;

    printf("Connecting to ZMQ queue...\n");

    void *context = zmq_ctx_new();
    void *requester = zmq_socket(context, ZMQ_SUB);
    int rc = zmq_connect(requester, zmqurl);
   
    // Subscribe to queue messages
    (void)zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    
    // Set receive time-out to 60 seconds
    int rcvto = 60 * 1000;
    (void)zmq_setsockopt(requester, ZMQ_RCVTIMEO, &rcvto, sizeof(rcvto));

    printf("Established context and socket with %s status\n", rc == 0 ? "OK" : "NOT OK");

    // Avoid that unitialized entries in pipeline are used
    for (int i = 0; i < SPOTSWINDOW; i++)
        pipeline[i].analyzed = true;

    for (int i = 0; i < MAXSKIMMERS; i++)
    {
        skimmer[i].active = false;
        skimmer[i].last = nowtime;
        for (int j = 0; j < BANDS; j++)
        {
            skimmer[i].band[j].active = false;
            skimmer[i].band[j].last = nowtime;
        }
    }

    if (debug)
    {
        // Clear screen of console
        printf("\033c");
    }

    while (!false) // Replace false with close down signal
    {
        int size = zmq_recv(requester, buffer, BUFLEN, 0);
        buffer[size] = 0;

        if (size > 9 && strncmp(buffer, "PROD_SPOT", 9) == 0)
        {
            if (!connected)
            {
                printf("Connected to ZMQ queue.\n");
                connected = true;
            }

            size = zmq_recv(requester, buffer, BUFLEN, 0);
            buffer[size] = 0;

            char de[STRLEN], dx[STRLEN], extradata[STRLEN];
            int snr, speed, spot_type, mode, ntp, got;
            unsigned long long int jstime1, jstime2;
            time_t spottime;
            double freq, base_freq;

            if (size > 0)
                got = sscanf(buffer, "%lf|%[^|]|%[^|]|%d|%lf|%d|%d|%d|%d|%lld|%lld|%s",
                    &freq, dx, de, &spot_type, &base_freq, &snr, &speed, &mode, &ntp, &jstime1, &jstime2, extradata);
            else
                got = 0;

            if (got == 12)
            {
				//printf("jstime1 = %lld jstime2 = %lld\n", jstime1, jstime2);

                spottime = jstime2 / 1000;

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
                        if (debug)
                            printstatuscall("F6IIT", "OK2EW", "DR4W", "SM6FMB", 4);

                        for (int pi = 0; pi < SPOTSWINDOW; pi++)
                        {
                            // delta is frequency deviation in kHz
                            double deltakhz = pipeline[pi].freq - freq;
                            double deltappm = 1.0E+06 * deltakhz / freq;
                            double adeltakhz = fabs(deltakhz);
                            double adeltappm = fabs(deltappm);

                            int bandindex = fqbandindex(freq);

                            // If pipeline entry...
                            if (!pipeline[pi].analyzed &&               // is not yet analyzed and
                                strcmp(pipeline[pi].dx, dx) == 0 &&     // has same call as reference spot and
                                adeltakhz <= MAXERRKHZ &&               // has low enough absolute deviation and
                                adeltappm < MAXERRPPM &&                // has low enough relative deviation and
                                strcmp(pipeline[pi].de, de) != 0 &&     // is not from same reference skimmer and
                                abs((int)difftime(pipeline[pi].time, spottime)) <= MAXAPART) // is close enough in time
                            {
                                // ..then we have a qualified spot in pipeline[pi]
                                // The data of the reference spot is in freq and de.

                                Qualifiedspots++;

                                pipeline[pi].analyzed = true; // To only analyze each spot once

                                // Check if this skimmer is already in list
                                int skimpos = -1;
                                for (int j = 0; j < Skimmers; j++)
                                {
                                    if (strcmp(pipeline[pi].de, skimmer[j].call) == 0)
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
                                    double factor = sqrt(freq / 14000.0) / (double)TC;

                                    skimmer[skimpos].band[bandindex].avdev =
                                        (1.0 - factor) * skimmer[skimpos].band[bandindex].avdev +
                                        factor * deltappm;

                                    // skimmer[skimpos].band[bandindex].avadj =
                                        // (1.0 - factor) * skimmer[skimpos].band[bandindex].avadj +
                                        // factor * pipeline[pi].freq / freq;

                                    skimmer[skimpos].band[bandindex].count++;
                                    skimmer[skimpos].band[bandindex].last = pipeline[pi].time;
                                    skimmer[skimpos].last = pipeline[pi].time;

                                    if (debug)
                                    {
                                        if (!skimmer[skimpos].band[bandindex].active)
                                        {
                                            sprintf(tmpstring, "Skimmer %s marked active on %s", skimmer[skimpos].call, 
                                                bandname[bandindex]);
                                            printstatus(tmpstring, 1);
                                        }
                                    }

                                    skimmer[skimpos].band[bandindex].active = true;
                                    skimmer[skimpos].active = true;

                                    // Calculate average error across bands.
                                    // Only include 10MHz and below if no higher band have spots
                                    double avsum = 0.0;
                                    int used = 0;
                                    for (int j = BANDS - 1; j >= 0; j--)
                                    {
                                        if (skimmer[skimpos].band[j].active && (j > 4 || used == 0))
                                        {
                                            avsum += skimmer[skimpos].band[j].avdev;
                                            used++;
                                        }
                                    }
                                    // It is safe to divide, we know used is never zero
                                    skimmer[skimpos].avdev = avsum / (double)used;

                                    if (debug)
                                    {
                                        sprintf(tmpstring, "%ld spots of which %.1lf%% qualified for analysis. Current rate is %.1lf spots per minute.        ", 
                                            Totalspots, 100.0 * (double)Qualifiedspots / (double)Totalspots, spotsperminute);
                                        printstatus(tmpstring, 2);

                                        if (skimpos < 85)
                                        {
                                            printf("\033[H");
                                            for (int si = 0; si < 85; si++)
                                            {
                                                int activebands = 0;
                                                for (int bi = 0; bi < BANDS; bi++)
                                                    activebands += skimmer[si].band[bi].active ? 1 : 0;

                                                if (skimmer[si].active)
                                                    printf("%10s:%+6.2lf(%d)", skimmer[si].call, skimmer[si].avdev, activebands);
                                                else
                                                {
                                                    if (si < Skimmers)
                                                        printf("%10s:%9s", skimmer[si].call, "");
                                                    else
                                                        printf("%10s %9s", "", "");
                                                }

                                                if ((si + 1) % 5 == 0)
                                                    printf("\n");
                                            }
                                            printf("\n");
                                        }
                                    }
                                }
                                else // If new skimmer, add it to list
                                {
                                    if (Skimmers >= MAXSKIMMERS)
                                    {
                                        fprintf(stderr, "Skimmer list overflow (%d). Clearing list.\n", Skimmers);
                                        Skimmers = 0;
                                    }
                                    strcpy(skimmer[Skimmers].call, pipeline[pi].de);
                                    // skimmer[Skimmers].band[bandindex].avadj = 1.0; // Guess zero error as start
                                    skimmer[Skimmers].band[bandindex].avdev = 0.0; // Guess zero error as start
                                    skimmer[Skimmers].avdev = 0.0;
                                    skimmer[Skimmers].band[bandindex].count = 1;
                                    skimmer[Skimmers].band[bandindex].last = pipeline[pi].time;
                                    skimmer[Skimmers].last = pipeline[pi].time;
                                    skimmer[Skimmers].reference = pipeline[pi].reference;
                                    skimmer[Skimmers].band[bandindex].active = true;
                                    skimmer[Skimmers].active = true;

                                    Skimmers++;
                                    // if (debug)
                                        // fprintf(stderr, "Found skimmer #%d: %s \n", Skimmers, pipeline[pi].de);
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

                    //printf("Adding spot %s time=%ld nowtime=%ld\n", de, spottime, nowtime);

                    // Advance pipeline pointer and wrap around at top of pipeline
                    spp = (spp + 1) % SPOTSWINDOW;
                }
            }
            else
            {
                if (debug && size > 0)
                    printf("Failed parsing of spot!\n");
                else
                    printf("Receive operation timed out!\n");                    
            }

            if (Totalspots % REPORTPERIOD == 0)
            {
	            if (debug)
				{
					printf("\033[2J"); // Clear screen
				}
				else
        	    {
                    struct tm curt = *gmtime(&nowtime);
                    int count = 0;
                    for (int i = 0; i < Skimmers; i++)
                        count += skimmer[i].active ? 1 : 0;
                    printf("%4d-%02d-%02d %02d:%02d:%02d UTC. Spot count: %ld. %.1lf spots/minute from %d skimmers.\n",
                        curt.tm_year + 1900, curt.tm_mon + 1,curt.tm_mday, curt.tm_hour, 
                        curt.tm_min, curt.tm_sec, Totalspots, spotsperminute, count);
                }
            }
        }

        // Check for inactive skimmers every fifteen seconds
        time(&nowtime);
        double elapsed = difftime(nowtime, lasttime);
        if (elapsed > 15.0) // Four times per minute
        {
            lasttime = nowtime;

            // Estimate spots per minute. Filter with tc=20 15 second periods.
            int periodcount = Totalspots - lastspotcount;
            spotsperminute = (19.0 * spotsperminute + 60.0 * periodcount / (double)elapsed) / 20.0;
            lastspotcount = Totalspots;

            // Walk through all identified skimmers for all bands
            // Check if we reached inactivity timer on each band
            // Check if we reached inactivity timer on all bands
            for (int si = 0; si < Skimmers; si++)
            {
                bool skimactive = false;
                for (int bi = 0; bi < BANDS; bi++)
                {
                    if (skimmer[si].band[bi].active && difftime(nowtime, skimmer[si].band[bi].last) >= MAXINACT)
                    {
                        skimmer[si].band[bi].active = false;
                        if (debug)
                        {
                            sprintf(tmpstring, "Skimmer %s marked inactive on %s - no spots for %.0f seconds         ",
                                skimmer[si].call, bandname[bi],
                                difftime(nowtime, skimmer[si].band[bi].last));
                            printstatus(tmpstring, 0);
                        }
                    }
                    skimactive |= skimmer[si].band[bi].active;
                }
                if (!skimactive && skimmer[si].active && debug)
                {
                    sprintf(tmpstring, "Skimmer %s marked inactive - no spots for %.0f seconds                ",
                        skimmer[si].call, difftime(nowtime, skimmer[si].last));
                    printstatus(tmpstring, 0);
                }
                skimmer[si].active = skimactive;
            }
        }

        // Read updated list of reference skimmers once per day
        struct tm curt = *gmtime(&nowtime);
        if (curt.tm_hour == REFUPDHOUR && curt.tm_min > REFUPDMINUTE && curt.tm_mday != lastday)
        // if (curt.tm_min > 30 && curt.tm_hour != lastday)
        {
            updatereferences();
            lastday = curt.tm_mday;
            // lastday = curt.tm_hour;
            sprintf(tmpstring,
                "Updated reference skimmer list %4d-%02d-%02d %02d:%02d:%02d UTC      ",
                curt.tm_year + 1900, curt.tm_mon + 1,curt.tm_mday,
                curt.tm_hour, curt.tm_min, curt.tm_sec);
            if (debug)
                printstatus(tmpstring, 3);
            else
                printf("%s\n", tmpstring);
        }

        // If the spots counter reaches maximum, reset counters and clear pipeline
        // but leave skimmer list including averages intact
        // LONG_MAX is half of ULONG_MAX so the check is safe
        if (Totalspots >= LONG_MAX)
        {
            Totalspots = 0;
            Qualifiedspots = 0;
            for (int i = 0; i < SPOTSWINDOW; i++)
                pipeline[i].analyzed = true;
            spp = 0;
        }
    }

    zmq_close(requester);
    zmq_ctx_destroy(context);

    return 0;
}
