#include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <stdbool.h>
// #include <time.h>
#include <limits.h>


int main(int argc, char *argv[])
{
    unsigned long int unsignedlongint = ULONG_MAX;
    long int longint = LONG_MAX;
    
    printf("max unsigned long int = %lu\nmax long int = %ld\n", unsignedlongint, longint);

    return 0;
}
