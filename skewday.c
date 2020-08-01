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
#define MINSPOTS 5
// Maximum difference from reference spot times 100Hz
#define MAXERR 5
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

int main(int argc, char *argv[])
{
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
        int quality;        // Estimate quality metric
        time_t first;       // Earliest spot
        time_t last;        // Latest spot
    };

    struct Skimmer
    {
        char call[STRLEN]; // Skimmer callsign
        bool reference;    // If a reference skimmer
        struct Bandinfo band[BANDS];
    };

    FILE   *fp, *fr;
    time_t firstspot, lastspot, nowtime;
    struct tm stime;
    char   filename[LINELEN] = "", line[LINELEN] = "",
           referenceskimmer[MAXREF][STRLEN], *spotmode = "CW",
           reffilename[STRLEN] = REFFILENAME, pbuffer[BUFLEN],
           avdevs[STRLEN], quals[STRLEN], counts[STRLEN], tmps[STRLEN];
           // Human friendly names of bands
    const char *bandname[] = BANDNAMES;
    bool   verbose = false, reference;
    int    i, j, c, referenceskimmers = 0, totalspots = 0, usedspots = 0,
           spp = 0, refspots = 0, minsnr = MINSNR, skimmers = 0,
           minspots = MINSPOTS, maxapart = MAXAPART;

    static struct Spot pipeline[SPOTSWINDOW];
    static struct Skimmer skimmer[MAXSKIMMERS], temp;

    // Avoid that unitialized entries in pipeline are used
    for (i = 0; i < SPOTSWINDOW; i++)
        pipeline[i].analyzed = true;

    while ((c = getopt(argc, argv, "drx:f:m:n:c:")) != -1)
    {
        switch (c)
        {
            case 'c': // Reference filename
                strcpy(reffilename, optarg);
                break;
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
        return 1;
    }

    fr = fopen(reffilename, "r");

    if (fr == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", reffilename);
        return 1;
    }

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
                    (void)fclose(fr);
                    return 1;
                }
            }
        }
    }

    (void)fclose(fr);

    void *pcontext = zmq_ctx_new();
    void *publisher = zmq_socket(pcontext, ZMQ_PUB);
    int trc = zmq_bind(publisher, ZMQPUBURL);

    printf("Established publisher context and socket with %s status\n", trc == 0 ? "OK" : "NOT OK");

    fp = fopen(filename, "r");

    if (fp == NULL)
    {
        fprintf(stderr, "Can not open file \"%s\". Abort.\n", filename);
        return 1;
    }

    printf("Opened RBN archive file %s\n", filename);

    while (fgets(line, LINELEN, fp) != NULL)
    {
        char de[STRLEN], dx[STRLEN], timestring[LINELEN], mode[STRLEN];
        double freq;
        int snr;
        time_t spottime;

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
                for (i = 0; i < referenceskimmers; i++)
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

                    for (i = 0; i < SPOTSWINDOW; i++)
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
                                for (j = 0; j < skimmers; j++)
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
                                        return 1;
                                    }
                                    strcpy(skimmer[skimmers].call, pipeline[i].de);
                                    skimmer[skimmers].band[bi].accadj = pipeline[i].freq / (10.0 * freq);
                                    skimmer[skimmers].band[bi].count = 1;
                                    skimmer[skimmers].band[bi].first = pipeline[i].time;
                                    skimmer[skimmers].band[bi].last = pipeline[i].time;
                                    skimmer[skimmers].reference = pipeline[i].reference;
                                    skimmers++;
                                    if (verbose)
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
        for (int bi = 0; bi < BANDS; bi++)
        {
            skimmer[si].band[bi].avdev = 1000000.0 * (skimmer[si].band[bi].accadj / skimmer[si].band[bi].count - 1.0);
            // Attempt for quality metric. 20 spots => 100 spots => 6
            if (skimmer[si].band[bi].count > 0)
            {
                int quality = (int)round(3.0 * log10(skimmer[si].band[bi].count));
                skimmer[si].band[bi].quality = (quality > 9.0) ? 9 : quality;
            }
            else
            {
                skimmer[si].band[bi].quality = 0;
            }
        }
    }

    // Sort by callsign (bubble)
    for (i = 0; i < skimmers - 1; ++i)
    {
        for (j = 0; j < skimmers - 1 - i; ++j)
        {
            if (strcmp(skimmer[j].call, skimmer[j + 1].call) > 0)
            {
                temp = skimmer[j + 1];
                skimmer[j + 1] = skimmer[j];
                skimmer[j] = temp;
            }
        }
    }

    // Print results
    char firsttimestring[LINELEN], lasttimestring[LINELEN];
    stime = *localtime(&firstspot);
    (void)strftime(firsttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);
    stime = *localtime(&lastspot);
    (void)strftime(lasttimestring, LINELEN, "%Y-%m-%d %H:%M", &stime);

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
    for (int bi = 0; bi < BANDS; bi++)
        printf("%10s", bandname[bi]);
    printf("\n");

    for (int bi = 0; bi < 4 * BANDS + 9; bi++)
        printf("-");
    printf("\n");

    for (int si = 0; si < skimmers; si++)
    {
        printf("%-9s", skimmer[si].call);
        for (int bi = 0; bi < BANDS; bi++)
        {
            if (skimmer[si].band[bi].count != 0)
                printf("%+7.2f(%d)", skimmer[si].band[bi].avdev, skimmer[si].band[bi].quality);
            else
                printf("          ");
        }
        printf("\n");
    }

    time(&nowtime);

    for (int si = 0; si < skimmers; si++)
    {
        strcpy(pbuffer, "SKEW_TEST_24H");
        printf("%s ", pbuffer);
        zmq_send(publisher, pbuffer, strlen(pbuffer), ZMQ_SNDMORE);        

        snprintf(pbuffer, BUFLEN, "{\"node\":\"%s\",\"time\":%ld,\"24h_per_band\":{", 
            skimmer[si].call, nowtime);
        int bp = strlen(pbuffer);
        bool first = true;
        for (int bi = 0; bi < BANDS; bi++)
        {
            if (skimmer[si].band[bi].count > 0)
            {
                snprintf(avdevs, STRLEN, "%.2f", skimmer[si].band[bi].avdev);
                snprintf(quals, STRLEN, "%d", skimmer[si].band[bi].quality);
                snprintf(counts, STRLEN, "%d", skimmer[si].band[bi].count);
            }
            else
            {
                strcpy(avdevs, "null");
                strcpy(quals, "null");
                strcpy(counts, "null");
            }            

            snprintf(tmps, BUFLEN, "%s%s:{\"%s,%s,%s}", first ? "" : ",",
                bandname[bi], avdevs, quals, counts);
            strcpy(&pbuffer[bp], tmps);
            bp += strlen(tmps);
            first = false;
        }
        pbuffer[bp++] = '}';
        pbuffer[bp++] = '}';
        pbuffer[bp] = '\0';
        zmq_send(publisher, pbuffer, bp, 0);
        printf("%s\n", pbuffer);
    }

    zmq_close(publisher);
    zmq_ctx_destroy(pcontext);

    return 0;
}
