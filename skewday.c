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
// Number of most recent spots considered in analysis
#define SPOTSWINDOW 1000
// Maximum number of reference skimmers
#define MAXREF 50
// Maximum number of skimmers supported
#define MAXSKIMMERS 400
// Usage string
#define USAGE "Usage: %s -f file [-dshqrw] [-t call] [-n N] [-m N] [-x sec] [-c file]\n"
// Max number of seconds apart from a reference spot
#define MAXAPART 30
// Minimum SNR required for spot to be used
#define MINSNR 3
// Minimum frequency for spot to be used
#define MINFREQ 1000
// Minimum number of spots to be analyzed
#define MINSPOTS 10
// Maximum difference from reference spot times 100Hz
#define MAXERR 5
// Minimum number of spots to become considered for a reference skimmer
#define MINREFSPOTS 200
// Name of file containing callsigns of anchor skimmmers
#define ANCHORSFILENAME  "anchors"
// Name of file containing callsigns of RTTY anchor skimmmers
#define RANCHORSFILENAME  "ranchors"
// Name of file containing callsigns of reference skimmmers
#define REFFILENAME "reference"
// Name of file containing callsigns of RTTY reference skimmmers
#define RREFFILENAME "rreference"
// Mode of spots
#define MODE "CW"

#define BANDNAMES {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m" }
#define BANDS 12
#define BUFLEN 1024
#define ZMQPUBURL "tcp://*:5568"

struct Spot
{
    char de[STRLEN];   // Skimmer callsign
    char dx[STRLEN];   // Spotted call
    time_t time;       // Spot timestamp in epoch format
    int snr;           // SNR for spotcount
    int freq;          // 10x spot frequency
    bool reference;    // Originates from a reference skimmer
    bool analyzed;     // Already analyzed
};

struct Bandinfo
{
    int count;          // Number of analyzed spots for this band
    double accadj;      // Accumulated adjustment factor for this band
    double avdev;       // Averaged deviation in ppm for this band
    int quality;        // Skew estimate quality metric
    time_t first;       // Earliest spot
    time_t last;        // Latest spot
};

struct Skimmer
{
    char call[STRLEN];  // Skimmer callsign
    int count;          // Total spot count for skimmer
    double avdev;       // Weighted average of deviation for skimmer
    double absavdev;    // Absolute value of avdev
    bool reference;     // If a reference skimmer
    int quality;        // Skew estimate quality metric
    struct Bandinfo band[BANDS];
};

static struct Spot pipeline[SPOTSWINDOW];
static struct Skimmer skimmer[MAXSKIMMERS];
static int refspots, skimmers, referenceskimmers, totalspots, usedspots;
static time_t firstspot, lastspot;
static int minsnr = MINSNR, minspots = MINSPOTS, maxapart = MAXAPART;
static char referenceskimmer[MAXREF][STRLEN], *spotmode = MODE;
static bool verbose = false;

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
        int quality = (int)round(9.0 * log10(count) / log10(2000.0));
        return quality > 9.0 ? 9 : quality;
    }
    else
    {
        return -1;
    }
}

void analyze(char *filename, char *reffilename)
{
    int spp = 0, snr;
    char de[STRLEN], dx[STRLEN], timestring[LINELEN], mode[STRLEN];
    double freq;
    time_t spottime;
    char line[LINELEN];
    struct tm stime;
    bool reference;
    FILE *fp, *fr;

    fr = fopen(reffilename, "r");

    if (fr == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", reffilename);
        abort();
    }

    referenceskimmers = 0;

    while (fgets(line, LINELEN, fr) != NULL)
    {
        char tempstring[LINELEN];
        if (sscanf(line, "%s", tempstring) == 1)
        {
            // Don't include comments
            if (tempstring[0] != '#')
            {
                strcpy(referenceskimmer[referenceskimmers], tempstring);
                referenceskimmers++;
                if (referenceskimmers >= MAXREF)
                {
                    fprintf(stderr, "Overflow: More than %d reference skimmers defined.\n", MAXREF);
                    break;
                }
            }
        }
    }

    (void)fclose(fr);

    refspots = 0;
    skimmers = 0;
    totalspots = 0;
    usedspots = 0;

    // Avoid that unitialized entries in pipeline are used
    for (int pi = 0; pi < SPOTSWINDOW; pi++)
        pipeline[pi].analyzed = true;

    for (int si = 0; si < MAXSKIMMERS; si++)
    {
        skimmer[si].count = 0;
        for (int bi = 0; bi < BANDS; bi++)
        {
            skimmer[si].band[bi].count = 0;
            skimmer[si].band[bi].accadj = 0.0;
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
        // callsign,de_pfx,de_cont,freq,band,dx,dx_pfx,dx_cont,mode,db,date,speed,tx_mode
        int got = sscanf(line, "%[^,],%*[^,],%*[^,],%lf,%*[^,],%[^,],%*[^,],%*[^,],%*[^,],%d,%[^,],%*[^,],%s",
            de, &freq, dx, &snr, timestring, mode);

        if (got == 6 ) // If parsing is successful
        {
            (void)strptime(timestring, FMT, &stime);
            spottime = mktime(&stime);

            if (totalspots++ == 0) // If first spot
            {
                lastspot = spottime;
                firstspot = spottime;
            }
            else
            {
                lastspot = spottime > lastspot ? spottime : lastspot;
                firstspot = spottime < firstspot ? spottime : firstspot;
            }

            // If SNR is sufficient and frequency OK and mode is right
            if (snr >= minsnr && freq >= MINFREQ && strcmp(mode, spotmode) == 0)
            {
                reference = false;

                // Check if this spot is from a reference skimmer
                for (int i = 0; i < referenceskimmers; i++)
                {
                    if (strcmp(de, referenceskimmer[i]) == 0)
                    {
                        reference = true;
                        break;
                    }
                }

                // If it is reference spot, use it to check all un-analyzed,
                // non-reference spots in the pipeline
                if (reference)
                {
                    refspots++;
                    int bi = fqbandindex(freq);

                    for (int i = 0; i < SPOTSWINDOW; i++)
                    {
                        if (!pipeline[i].analyzed &&
                            strcmp(pipeline[i].dx, dx) == 0 &&
                            abs((int)difftime(pipeline[i].time, spottime)) <= maxapart)
                        {
                            int delta = pipeline[i].freq - (int)round(freq * 10.0);
                            int adelta = delta > 0 ? delta : -delta;

                            pipeline[i].analyzed = true; // To only analyze each spot once

                            if (adelta <= MAXERR) // Only consider spots less than MAXERR off from reference skimmer
                            {
                                usedspots++;

                                // Check if this skimmer is already in list
                                int skimpos = -1;
                                for (int j = 0; j < skimmers; j++)
                                {
                                    if (strcmp(pipeline[i].de, skimmer[j].call) == 0)
                                    {
                                        skimpos = j;
                                        break;
                                    }
                                }

                                if (skimpos != -1) // if in the list, update it
                                {
                                    skimmer[skimpos].band[bi].accadj += pipeline[i].freq / (10.0 * freq);
                                    skimmer[skimpos].band[bi].count++;
                                    if (pipeline[i].time > skimmer[skimpos].band[bi].last)
                                        skimmer[skimpos].band[bi].last = pipeline[i].time;
                                    if (pipeline[i].time < skimmer[skimpos].band[bi].first)
                                        skimmer[skimpos].band[bi].first = pipeline[i].time;
                                }
                                else // If new skimmer, add it to list
                                {
                                    if (skimmers >= MAXSKIMMERS) {
                                        fprintf(stderr, "Overflow: More than %d skimmers found.\n", MAXSKIMMERS);
                                        (void)fclose(fp);
                                        abort();
                                    }
                                    strcpy(skimmer[skimmers].call, pipeline[i].de);
                                    skimmer[skimmers].band[bi].accadj = pipeline[i].freq / (10.0 * freq);
                                    skimmer[skimmers].band[bi].count = 1;
                                    skimmer[skimmers].band[bi].first = pipeline[i].time;
                                    skimmer[skimmers].band[bi].last = pipeline[i].time;
                                    skimmer[skimmers].reference = pipeline[i].reference;
                                    skimmers++;
                                    if (verbose && false)
                                        fprintf(stderr, "Found skimmer #%d: %s \n", skimmers, pipeline[i].de);
                                }
                            }
                        }
                    }
                }

                // Save new spot in pipeline
                strcpy(pipeline[spp].de, de);
                strcpy(pipeline[spp].dx, dx);
                pipeline[spp].freq = (int)round(freq * 10.0);
                pipeline[spp].snr = snr;
                pipeline[spp].reference = reference;
                pipeline[spp].analyzed = false;
                pipeline[spp].time = spottime;

                // Move pointer and wrap around at top of pipeline
                spp = (spp + 1) % SPOTSWINDOW;
            }
        }
    }

    (void)fclose(fp);

    // Calculate statistics
    for (int si = 0; si < skimmers; si++)
    {
        skimmer[si].count = 0;
        for (int bi = 0; bi < BANDS; bi++)
        {
            skimmer[si].band[bi].avdev = 1000000.0 * (skimmer[si].band[bi].accadj / skimmer[si].band[bi].count - 1.0);
            if (skimmer[si].band[bi].count >= minspots)
            {
                skimmer[si].count += skimmer[si].band[bi].count;
                skimmer[si].band[bi].quality = qualmetric(skimmer[si].band[bi].count);
            }
        }
        if (skimmer[si].count >= minspots)
            skimmer[si].quality = qualmetric(skimmer[si].count);
    }

    // Calculate average error across bands.
    // Only include 5MHz and below if no higher band have spots
    // Weight bands by spot count since accumulated deviation is summed
    // 0 = 160m, 1 = 80m, 2 = 60m, 3 = 40m, 4 = 30m, 5 = 20m
    for (int si = 0; si < skimmers; si++)
    {
        double accadjsum = 0.0;
        int usedspots  = 0;
        for (int bi = BANDS - 1; bi >= 0; bi--)
        {
            if (skimmer[si].band[bi].count > 0 && (bi > 2 || usedspots == 0))
            {
                accadjsum += skimmer[si].band[bi].accadj;
                usedspots += skimmer[si].band[bi].count;
            }
        }

        // It is safe to divide, we know usedspots is never zero
        skimmer[si].avdev = 1000000.0 * (accadjsum / (double)usedspots - 1.0);
        skimmer[si].absavdev = fabs(skimmer[si].avdev);
    }

    // Sort by callsign (bubble)
    // struct Skimmer temp;
    // for (int i = 0; i < skimmers - 1; ++i)
    // {
    //     for (int j = 0; j < skimmers - 1 - i; ++j)
    //     {
    //         if (strcmp(skimmer[j].call, skimmer[j + 1].call) > 0)
    //         {
    //             temp = skimmer[j + 1];
    //             skimmer[j + 1] = skimmer[j];
    //             skimmer[j] = temp;
    //         }
    //     }
    // }

    // Sort by absolute deviation (bubble)
    struct Skimmer temp;
    for (int i = 0; i < skimmers - 1; ++i)
    {
        for (int j = 0; j < skimmers - 1 - i; ++j)
        {
            if (skimmer[j].absavdev > skimmer[j + 1].absavdev)
            {
                temp = skimmer[j + 1];
                skimmer[j + 1] = skimmer[j];
                skimmer[j] = temp;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    time_t  nowtime;
    char    filename[LINELEN] = "", pbuffer[BUFLEN],
            reffilename[STRLEN] = REFFILENAME,
            anchfilename[STRLEN] = ANCHORSFILENAME,
            avdevs[STRLEN], quals[STRLEN], counts[STRLEN], tmps[LINELEN];
    const char *bandname[] = BANDNAMES;
    int     c;
    FILE *fr;

    while ((c = getopt(argc, argv, "drx:f:m:n:c:")) != -1)
    {
        switch (c)
        {
            case 'f': // Filename
                strcpy(filename, optarg);
                break;
            case 'd': // Verbose debug mode
                verbose = true;
                break;
            case 'm': // Minimum number of spots to consider skimmer
                minspots = atoi(optarg);
                break;
            case 'n': // Minimum SNR to consider spot
                minsnr = atoi(optarg);
                break;
            case 'x': // Maximum difference in time to a reference spot
                maxapart = atoi(optarg);
                break;
            case 'r': // RTTY mode
                spotmode = "RTTY";
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

    fprintf(fr, "# Automatically generated reference skimmer list\n");

    fprintf(fr, "# Skimmers with < 0.1ppm deviation from anchor skimmers\n");
    for (int si = 0; si < skimmers; si++)
    {
        if (skimmer[si].absavdev < 0.1 && skimmer[si].count >= MINREFSPOTS)
        {
            // fprintf(fr, "%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            printf("%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            fprintf(fr, "%s\n", skimmer[si].call);
        }
    }

    fprintf(fr, "# Skimmers with < 0.2ppm deviation from anchor skimmers\n");
    for (int si = 0; si < skimmers; si++)
    {
        if (skimmer[si].absavdev >= 0.1 && skimmer[si].absavdev < 0.2 && skimmer[si].count >= MINREFSPOTS)
        {
            // fprintf(fr, "%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            printf("%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            fprintf(fr, "%s\n", skimmer[si].call);
        }
    }

    fprintf(fr, "# Skimmers with < 0.3ppm deviation from anchor skimmers\n");
    for (int si = 0; si < skimmers; si++)
    {
        if (skimmer[si].absavdev >= 0.2 && skimmer[si].absavdev < 0.3 && skimmer[si].count >= MINREFSPOTS)
        {
            // fprintf(fr, "%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            printf("%s = %.2f\n", skimmer[si].call, skimmer[si].avdev);
            fprintf(fr, "%s\n", skimmer[si].call);
        }
    }

    (void)fclose(fr);

    // Run analysis using updated list of reference skimmers
    printf("Analysis pass #2...\n");
    analyze(filename, reffilename);

    // Print results
    char firsttimestring[LINELEN], lasttimestring[LINELEN];
    struct tm stime;
    stime = *localtime(&firstspot);
    (void)strftime(firsttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);
    stime = *localtime(&lastspot);
    (void)strftime(lasttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);

	if (verbose)
	{
	    printf("%d RBN spots between %s and %s.\n", totalspots, firsttimestring, lasttimestring);
	    printf("%d spots (%.1f%%) were from reference skimmers (*).\n",  refspots, 100.0 * refspots / totalspots);

	    printf("Average spot flow was %.0f per minute from %d active %s skimmers.\n",
    	    60 * totalspots / difftime(lastspot, firstspot), skimmers, spotmode);

	    printf("%d spots from %d skimmers qualified for analysis by meeting\nthe following criteria:\n", usedspots, skimmers);
	    printf(" * Mode of spot is %s.\n" , spotmode);
	    printf(" * Also spotted by a reference skimmer within %d spots.\n", SPOTSWINDOW);
	    printf(" * Also spotted by a reference skimmer within %ds. \n", maxapart);
	    printf(" * SNR is %ddB or higher. \n", minsnr);
	    printf(" * Frequency is %dkHz or higher. \n", MINFREQ);
	    printf(" * Frequency deviation from reference skimmer is %.1fkHz or less.\n", MAXERR / 10.0);
	    printf(" * At least %d spots from same skimmer in data set.\n", minspots);

        // Present results for each skimmer
	    printf("%-9s", "Skimmer");
	    printf("%10s", "Total");
	    for (int bi = 0; bi < BANDS; bi++)
	        printf("%10s", bandname[bi]);
	    printf("\n");

	    for (int bi = 0; bi < 4 * BANDS + 9; bi++)
	        printf("-");
	    printf("\n");

	    for (int si = 0; si < skimmers; si++)
	    {
            if (skimmer[si].count > MINSPOTS)
            {
                strcpy(tmps, skimmer[si].call);
                strcat(tmps, skimmer[si].reference ? "*" : "");
                printf("%-9s", tmps);
                printf("%+7.2f(%d)", skimmer[si].avdev, skimmer[si].quality);

                for (int bi = 0; bi < BANDS; bi++)
                {
                    if (skimmer[si].band[bi].count > MINSPOTS)
                        printf("%+7.2f(%d)", skimmer[si].band[bi].avdev, skimmer[si].band[bi].quality);
                    else
                        printf("          ");
                }
                printf("\n");
            }
    	}
	}

    time(&nowtime);

    for (int si = 0; si < skimmers; si++)
    {
        if (skimmer[si].count >= minspots)
        {
            strcpy(pbuffer, "SKEW_TEST_24H");
            if (verbose) printf("%s ", pbuffer);
            zmq_send(publisher, pbuffer, strlen(pbuffer), ZMQ_SNDMORE);

            snprintf(pbuffer, BUFLEN, "{\"node\":\"%s\",\"ref\":%s,\"time\":%ld,\"24h_skew\":{%.2f,%d,%d}\"24h_per_band\":{",
                skimmer[si].call, skimmer[si].reference ? "true" : "false",
                nowtime, skimmer[si].avdev, skimmer[si].quality, skimmer[si].count);
            int bp = strlen(pbuffer);
            bool first = true;
            for (int bi = 0; bi < BANDS; bi++)
            {
                bool valid  = skimmer[si].band[bi].count >= minspots;

                if (valid)
                {
                    snprintf(avdevs, STRLEN, "%.2f", skimmer[si].band[bi].avdev);
                    snprintf(quals, STRLEN, "%d", skimmer[si].band[bi].quality);
                    snprintf(counts, STRLEN, "%d", skimmer[si].band[bi].count);
                }

                snprintf(tmps, LINELEN, "%s\"%s\":{%s,%s,%s}", first ? "" : ",", bandname[bi],
                    valid ? avdevs : "null", valid ? quals : "null", valid ? counts : "null");

                strcpy(&pbuffer[bp], tmps);
                bp += strlen(tmps);
                first = false;
            }

            pbuffer[bp++] = '}';
            pbuffer[bp++] = '}';
            pbuffer[bp] = '\0';

            zmq_send(publisher, pbuffer, bp, 0);
            if (verbose) printf("%s\n", pbuffer);
        }
    }

    zmq_close(publisher);
    zmq_ctx_destroy(pcontext);

    printf("Done processing and reporting\n");

    return 0;
}
