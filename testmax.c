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
    
    printf("max unsigned long int = %lu = %.2e\n", unsignedlongint, (double)unsignedlongint);
    printf("max long int = %ld = %.2e\n", longint, (double)longint);

    return 0;
}
