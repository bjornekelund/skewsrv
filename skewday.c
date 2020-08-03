#define _XOPEN_SOURCE
#include "zmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>

// Standard length of strings
#define STRLEN 16
// Standard length of lines
#define LINELEN 128
// Time format in RBN data file
#define FMT "%Y-%m-%d %H:%M:%S"
// Maximum number of reference skimmers
#define MAXREF 100
// Maximum number of skimmers supported
#define MAXSKIMMERS 500

// Number of most recent spots considered in analysis
#define SPOTSWINDOW 1000
// Usage string
#define USAGE "Usage: %s -f file [-dr] [-t call] [-n N] [-m N] [-x sec]\n"
// Max number of seconds apart from a reference spot
#define MAXAPART 30
// Minimum SNR required for spot to be used
#define MINSNR 6
// Minimum frequency for spot to be used
#define MINFREQ 1800
// Minimum number of spots to be report results
#define MINSPOTS 10
// Maximum difference from reference spot times in kHz
#define MAXERR 0.5
// Minimum number of spots to become considered for a reference skimmer
#define MINREFSPOTS 100
// Name of file containing callsigns of anchor skimmmers
#define ANCHORSFILENAME  "anchors"
// Name of file containing callsigns of RTTY anchor skimmmers
#define RANCHORSFILENAME  "ranchors"
// Name of file containing callsigns of reference skimmmers
#define REFFILENAME "reference"
// Name of file containing callsigns of RTTY reference skimmmers
#define RREFFILENAME "rreference"
// Default mode of spots
#define MODE "CW"
// Human friendly band names
#define BANDNAMES {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m" }
#define BANDS 12
// Size of output ZMQ output buffer
#define BUFLEN 1024
// ZMQ publication URL
#define ZMQPUBURL "tcp://*:5568"

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

struct NodeBand
{
    int count;          // Number of analyzed spots for this band
    double accadj;      // Accumulated adjustment factor for this band
    double avdev;       // Averaged deviation in ppm for this band
    int quality;        // Skew estimate quality metric
    time_t first;       // Earliest spot
    time_t last;        // Latest spot
};

struct Node
{
    char call[STRLEN];  // Skimmer callsign
    int count;          // Total spot count for skimmer
    double avdev;       // Weighted average of deviation for skimmer
    double absavdev;    // Absolute value of avdev
    bool reference;     // If a reference skimmer
    int quality;        // Consolidated skew estimate quality metric
    struct NodeBand band[BANDS];
};

static struct Spot Pipeline[SPOTSWINDOW];
static struct Node Skimmer[MAXSKIMMERS];
static int Refspots, Skimmers, ReferenceSkimmers, TotalSpots, Usedspots;
static time_t FirstSpot, LastSpot;
static int MinSNR = MINSNR, MinSpots = MINSPOTS, MaxApart = MAXAPART;
static char ReferenceSkimmer[MAXREF][STRLEN], *SpotMode = MODE;
static bool Verbose = false;

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

int qualmetric(int count)
{
    if (count > 0)
    {
        int quality = (int)(9.0 * log10(count) / log10(2000.0));
        return quality > 9.0 ? 9 : quality;
    }
    else
    {
        return -1;
    }
}

void analyze(char *filename, char *reffilename)
{
    int spp = 0;
    char line[LINELEN];
    time_t spottime;
    struct tm stime;
    FILE *fp, *fr;

    fr = fopen(reffilename, "r");

    if (fr == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", reffilename);
        abort();
    }

    ReferenceSkimmers = 0;

    while (fgets(line, LINELEN, fr) != NULL)
    {
        char tempstring[LINELEN];
        if (sscanf(line, "%s", tempstring) == 1)
        {
            // Don't include comments
            if (tempstring[0] != '#')
            {
                strcpy(ReferenceSkimmer[ReferenceSkimmers], tempstring);
                ReferenceSkimmers++;
                if (ReferenceSkimmers >= MAXREF)
                {
                    fprintf(stderr, "Overflow: More than %d reference skimmers defined.\n", MAXREF);
                    break;
                }
            }
        }
    }

    (void)fclose(fr);

    Refspots = 0;
    Skimmers = 0;
    TotalSpots = 0;
    Usedspots = 0;

    // Avoid that unitialized entries in pipeline are used
    for (int pi = 0; pi < SPOTSWINDOW; pi++)
        Pipeline[pi].analyzed = true;

    // Clear statistics matrix
    for (int si = 0; si < MAXSKIMMERS; si++)
    {
        Skimmer[si].count = 0;
        for (int bi = 0; bi < BANDS; bi++)
        {
            Skimmer[si].band[bi].count = 0;
            Skimmer[si].band[bi].accadj = 0.0;
        }
    }

    fp = fopen(filename, "r");

    if (fp == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", filename);
        abort();
    }

    while (fgets(line, LINELEN, fp) != NULL)
    {
        char de[STRLEN], dx[STRLEN], timestring[LINELEN], mode[STRLEN];
        double freq;
        int snr;
        bool reference;

        // callsign,de_pfx,de_cont,freq,band,dx,dx_pfx,dx_cont,mode,db,date,speed,tx_mode
        int got = sscanf(line, "%[^,],%*[^,],%*[^,],%lf,%*[^,],%[^,],%*[^,],%*[^,],%*[^,],%d,%[^,],%*[^,],%s",
            de, &freq, dx, &snr, timestring, mode);

        if (got == 6 ) // If parsing is successful
        {
            (void)strptime(timestring, FMT, &stime);
            spottime = mktime(&stime);

            if (TotalSpots++ == 0) // If first spot
            {
                LastSpot = spottime;
                FirstSpot = spottime;
            }
            else
            {
                LastSpot = spottime > LastSpot ? spottime : LastSpot;
                FirstSpot = spottime < FirstSpot ? spottime : FirstSpot;
            }

            // If SNR is sufficient and frequency OK and mode is right
            if (snr >= MinSNR && freq >= MINFREQ && strcmp(mode, SpotMode) == 0)
            {
                reference = false;

                // Check if this spot is from a reference skimmer
                for (int i = 0; i < ReferenceSkimmers; i++)
                {
                    if (strcmp(de, ReferenceSkimmer[i]) == 0)
                    {
                        reference = true;
                        break;
                    }
                }

                // If it is reference spot, use it to check all un-analyzed,
                // non-reference spots in the pipeline
                if (reference)
                {
                    Refspots++;
                    int bi = fqbandindex(freq);

                    for (int i = 0; i < SPOTSWINDOW; i++)
                    {
                        if (!Pipeline[i].analyzed &&
                            strcmp(Pipeline[i].dx, dx) == 0 &&
                            abs((int)difftime(Pipeline[i].time, spottime)) <= MaxApart)
                        {
                            double delta = Pipeline[i].freq - freq;
                            double adelta = fabs(delta);

                            Pipeline[i].analyzed = true; // To only analyze each spot once

                            if (adelta <= MAXERR) // Only consider spots less than MAXERR off from reference skimmer
                            {
                                Usedspots++;

                                // Check if this skimmer is already in list
                                int skimpos = -1;
                                for (int j = 0; j < Skimmers; j++)
                                {
                                    if (strcmp(Pipeline[i].de, Skimmer[j].call) == 0)
                                    {
                                        skimpos = j;
                                        break;
                                    }
                                }

                                if (skimpos != -1) // if in the list, update it
                                {
                                    Skimmer[skimpos].band[bi].accadj += Pipeline[i].freq / freq;
                                    Skimmer[skimpos].band[bi].count++;
                                    if (Pipeline[i].time > Skimmer[skimpos].band[bi].last)
                                        Skimmer[skimpos].band[bi].last = Pipeline[i].time;
                                    if (Pipeline[i].time < Skimmer[skimpos].band[bi].first)
                                        Skimmer[skimpos].band[bi].first = Pipeline[i].time;
                                }
                                else // If new skimmer, add it to list
                                {
                                    if (Skimmers >= MAXSKIMMERS) {
                                        fprintf(stderr, "Overflow: More than %d skimmers found.\n", MAXSKIMMERS);
                                        (void)fclose(fp);
                                        abort();
                                    }
                                    strcpy(Skimmer[Skimmers].call, Pipeline[i].de);
                                    Skimmer[Skimmers].band[bi].accadj = Pipeline[i].freq / freq;
                                    Skimmer[Skimmers].band[bi].count = 1;
                                    Skimmer[Skimmers].band[bi].first = Pipeline[i].time;
                                    Skimmer[Skimmers].band[bi].last = Pipeline[i].time;
                                    Skimmer[Skimmers].reference = Pipeline[i].reference;
                                    Skimmers++;
                                    if (Verbose && false)
                                        fprintf(stderr, "Found skimmer #%d: %s \n", Skimmers, Pipeline[i].de);
                                }
                            }
                        }
                    }
                }

                // Save new spot in pipeline
                strcpy(Pipeline[spp].de, de);
                strcpy(Pipeline[spp].dx, dx);
                Pipeline[spp].freq = freq;
                Pipeline[spp].snr = snr;
                Pipeline[spp].reference = reference;
                Pipeline[spp].analyzed = false;
                Pipeline[spp].time = spottime;

                // Move pointer and wrap around at top of pipeline
                spp = (spp + 1) % SPOTSWINDOW;
            }
        }
    }

    (void)fclose(fp);

    // Calculate statistics
    for (int si = 0; si < Skimmers; si++)
    {
        Skimmer[si].count = 0;
        for (int bi = 0; bi < BANDS; bi++)
        {
            if (Skimmer[si].band[bi].count > 0)
            {
                Skimmer[si].band[bi].avdev = 1000000.0 * (Skimmer[si].band[bi].accadj / Skimmer[si].band[bi].count - 1.0);
                Skimmer[si].count += Skimmer[si].band[bi].count;
                Skimmer[si].band[bi].quality = qualmetric(Skimmer[si].band[bi].count);
            }
        }
        Skimmer[si].quality = qualmetric(Skimmer[si].count);
    }

    // Calculate average error across bands.
    // Only include 5MHz and below if no higher band have spots
    // Weight band skews with spot count by averaging accumulated 
    // relative deviation for each band
    // 0 = 160m, 1 = 80m, 2 = 60m, 3 = 40m, 4 = 30m, 5 = 20m
    for (int si = 0; si < Skimmers; si++)
    {
        double accadjsum = 0.0;
        int usablespots  = 0;
        for (int bi = BANDS - 1; bi >= 0; bi--)
        {
            if (Skimmer[si].band[bi].count > 0 && (bi > 2 || usablespots == 0))
            {
                accadjsum += Skimmer[si].band[bi].accadj;
                usablespots += Skimmer[si].band[bi].count;
            }
        }

        // It is safe to divide, we know usablespots is never zero
        Skimmer[si].avdev = 1000000.0 * (accadjsum / (double)usablespots - 1.0);
        Skimmer[si].absavdev = fabs(Skimmer[si].avdev);
    }

    // Sort by absolute deviation (bubble)
    struct Node temp;
    for (int i = 0; i < Skimmers - 1; ++i)
    {
        for (int j = 0; j < Skimmers - 1 - i; ++j)
        {
    //         if (strcmp(Skimmer[j].call, Skimmer[j + 1].call) > 0)
            if (Skimmer[j].absavdev > Skimmer[j + 1].absavdev)
            {
                temp = Skimmer[j + 1];
                Skimmer[j + 1] = Skimmer[j];
                Skimmer[j] = temp;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    char    filename[LINELEN] = "", pbuffer[BUFLEN],
            reffilename[STRLEN] = REFFILENAME,
            anchfilename[STRLEN] = ANCHORSFILENAME,
            avdevs[STRLEN], quals[STRLEN], counts[STRLEN], tmps[LINELEN];
    const char *bandname[] = BANDNAMES;
    int     c;
    FILE *fr;

    while ((c = getopt(argc, argv, "drx:f:m:n:")) != -1)
    {
        switch (c)
        {
            case 'f': // Filename
                strcpy(filename, optarg);
                break;
            case 'd': // Verbose debug mode
                Verbose = true;
                break;
            case 'm': // Minimum number of spots to consider skimmer
                MinSpots = atoi(optarg);
                break;
            case 'n': // Minimum SNR to consider spot
                MinSNR = atoi(optarg);
                break;
            case 'x': // Maximum difference in time to a reference spot
                MaxApart = atoi(optarg);
                break;
            case 'r': // RTTY mode
                SpotMode = "RTTY";
                strcpy(reffilename, RREFFILENAME);
                strcpy(anchfilename, RANCHORSFILENAME);
                break;
            case '?':
                fprintf(stderr, USAGE, argv[0]);
                return 1;
            default:
                abort();
        }
    }

    if (strlen(filename) == 0)
    {
        fprintf(stderr, USAGE, argv[0]);
        abort();
    }

    void *pcontext = zmq_ctx_new();
    void *publisher = zmq_socket(pcontext, ZMQ_PUB);
    int trc = zmq_bind(publisher, ZMQPUBURL);

    printf("Established publisher context and socket with %s status\n", trc == 0 ? "OK" : "NOT OK");

    printf("Analysis pass #1...\n");

    // Run analysis using anchor skimmers
    analyze(filename, anchfilename);

    fr = fopen(reffilename, "w");

    if (fr == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", reffilename);
        abort();
    }

    // Create new reference file for both own and other use
    char firsttimestring[LINELEN], lasttimestring[LINELEN];
    struct tm stime;
    stime = *localtime(&FirstSpot);
    (void)strftime(firsttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);
    stime = *localtime(&LastSpot);
    (void)strftime(lasttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);

    fprintf(fr, "# Machine generated reference skimmer list based on\n");
    fprintf(fr, "# %d RBN spots between %s and %s.\n", TotalSpots, firsttimestring, lasttimestring);
    fprintf(fr, "# Only skimmers with more than %d qualified spots are considered.\n#\n", MINREFSPOTS);

    fprintf(fr, "# Skimmers with < 0.1ppm deviation from anchor skimmers.\n");
    for (int si = 0; si < Skimmers; si++)
    {
        if (Skimmer[si].absavdev < 0.1 && Skimmer[si].count >= MINREFSPOTS)
        {
            if (Verbose) printf("%s(%.2f) ", Skimmer[si].call, Skimmer[si].avdev);
            fprintf(fr, "%s\n", Skimmer[si].call);
        }
    }
    if (Verbose) printf("\n");

    fprintf(fr, "# Skimmers with < 0.2ppm deviation from anchor skimmers.\n");
    for (int si = 0; si < Skimmers; si++)
    {
        if (Skimmer[si].absavdev >= 0.1 && Skimmer[si].absavdev < 0.2 && Skimmer[si].count >= MINREFSPOTS)
        {
            if (Verbose) printf("%s(%.2f) ", Skimmer[si].call, Skimmer[si].avdev);
            fprintf(fr, "%s\n", Skimmer[si].call);
        }
    }
    if (Verbose) printf("\n");

    fprintf(fr, "# Skimmers with < 0.3ppm deviation from anchor skimmers.\n");
    for (int si = 0; si < Skimmers; si++)
    {
        if (Skimmer[si].absavdev >= 0.2 && Skimmer[si].absavdev < 0.3 && Skimmer[si].count >= MINREFSPOTS)
        {
            if (Verbose) printf("%s(%.2f) ", Skimmer[si].call, Skimmer[si].avdev);
            fprintf(fr, "%s\n", Skimmer[si].call);
        }
    }
    if (Verbose) printf("\n");

    (void)fclose(fr);

    // Run analysis using updated list of reference skimmers
    printf("Analysis pass #2...\n");
    analyze(filename, reffilename);

    // Print results
	if (Verbose)
	{
	    printf("%d RBN spots between %s and %s.\n", TotalSpots, firsttimestring, lasttimestring);
	    printf("%d spots (%.1f%%) were from reference skimmers (*).\n",  Refspots, 100.0 * Refspots / TotalSpots);

	    printf("Average spot flow was %.0f per minute from %d active %s skimmers.\n",
    	    60 * TotalSpots / difftime(LastSpot, FirstSpot), Skimmers, SpotMode);

	    printf("%d spots from %d skimmers qualified for analysis by meeting\nthe following criteria:\n", Usedspots, Skimmers);
	    printf(" * Mode of spot is %s.\n" , SpotMode);
	    printf(" * Also spotted by a reference skimmer within %d spots.\n", SPOTSWINDOW);
	    printf(" * Also spotted by a reference skimmer within %ds. \n", MaxApart);
	    printf(" * SNR is %ddB or higher. \n", MinSNR);
	    printf(" * Frequency is %dkHz or higher. \n", MINFREQ);
	    printf(" * Frequency deviation from reference skimmer is %.1fkHz or less.\n", MAXERR / 10.0);
	    printf(" * At least %d spots from same skimmer in data set.\n", MinSpots);

        // Present results for each skimmer
	    printf("%-9s", "Skimmer");
	    printf("%9s", "Total");
	    for (int bi = 0; bi < BANDS; bi++)
	        printf("%9s", bandname[bi]);
	    printf("\n");

	    for (int bi = 0; bi < 4 * BANDS + 9; bi++)
	        printf("-");
	    printf("\n");

	    for (int si = 0; si < Skimmers; si++)
	    {
            if (Skimmer[si].count > MINSPOTS)
            {
                strcpy(tmps, Skimmer[si].call);
                strcat(tmps, Skimmer[si].reference ? "*" : "");
                printf("%-9s", tmps);
                printf("%+6.1f(%d)", Skimmer[si].avdev, Skimmer[si].quality);

                for (int bi = 0; bi < BANDS; bi++)
                {
                    if (Skimmer[si].band[bi].count > MINSPOTS)
                        printf("%+6.1f(%d)", Skimmer[si].band[bi].avdev, Skimmer[si].band[bi].quality);
                    else
                        printf("%9s", "");
                }
                printf("\n");
            }
    	}
	}

    for (int si = 0; si < Skimmers; si++)
    {
        if (Skimmer[si].count >= MinSpots)
        {
            strcpy(pbuffer, "SKEW_TEST_24H");
            if (Verbose) printf("%s ", pbuffer);
            zmq_send(publisher, pbuffer, strlen(pbuffer), ZMQ_SNDMORE);

            time_t  nowtime;
            time(&nowtime);

            snprintf(pbuffer, BUFLEN, "{\"node\":\"%s\",\"ref\":%s,\"time\":%ld,\"24h_skew\":{\"skew\":%.1f,\"qual\":%d,\"count\":%d}\"24h_per_band\":{",
                Skimmer[si].call, Skimmer[si].reference ? "true" : "false",
                nowtime, Skimmer[si].avdev, Skimmer[si].quality, Skimmer[si].count);
            int bp = strlen(pbuffer);
            bool first = true;
            for (int bi = 0; bi < BANDS; bi++)
            {
                bool valid  = Skimmer[si].band[bi].count >= MinSpots;

                if (valid)
                {
                    snprintf(avdevs, STRLEN, "%.1f", Skimmer[si].band[bi].avdev);
                    snprintf(quals, STRLEN, "%d", Skimmer[si].band[bi].quality);
                    snprintf(counts, STRLEN, "%d", Skimmer[si].band[bi].count);
                }

                snprintf(tmps, LINELEN, "%s\"%s\":{\"skew\":%s,\"qual\":%s,\"count\":%s}", first ? "" : ",", bandname[bi],
                    valid ? avdevs : "null", valid ? quals : "null", valid ? counts : "null");

                strcpy(&pbuffer[bp], tmps);
                bp += strlen(tmps);
                first = false;
            }

            pbuffer[bp++] = '}';
            pbuffer[bp++] = '}';
            pbuffer[bp] = '\0';

            zmq_send(publisher, pbuffer, bp, 0);
            if (Verbose) printf("%s\n", pbuffer);
        }
    }

    zmq_close(publisher);
    zmq_ctx_destroy(pcontext);

    printf("Done processing and reporting\n");

    return 0;
}
