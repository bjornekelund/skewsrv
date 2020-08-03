#include <stdio.h>
#include <limits.h>

int main(int argc, char *argv[])
{
    unsigned int unsignedint = UINT_MAX;
    int signedint = INT_MAX;
    unsigned long int unsignedlongint = ULONG_MAX;
    long int longint = LONG_MAX;
    
    printf("max unsigned int = %u = %.2e (%zu bytes)\n", unsignedint, (double)unsignedint, sizeof(unsignedint));
    printf("max int = %d = %.2e (%zu bytes)\n", signedint, (double)signedint, sizeof(signedint));
    printf("max unsigned long int = %lu = %.2e (%zu bytes)\n", unsignedlongint, (double)unsignedlongint, sizeof(unsignedlongint));
    printf("max long int = %ld = %.2e (%zu bytes)\n", longint, (double)longint, sizeof(longint));

    return 0;
}
