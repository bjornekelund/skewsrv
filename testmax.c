#include <stdio.h>
#include <limits.h>

int main(int argc, char *argv[])
{
    unsigned int unsignedint = UINT_MAX;
    int signedint = INT_MAX;
    unsigned long int unsignedlongint = ULONG_MAX;
    long int longint = LONG_MAX;
    
    printf("max unsigned int = %u = %.2e\n", unsignedint, (double)unsignedint);
    printf("max int = %d = %.2e\n", signedint, (double)signedint);
    printf("max unsigned long int = %lu = %.2e\n", unsignedlongint, (double)unsignedlongint);
    printf("max long int = %ld = %.2e\n", longint, (double)longint);

    return 0;
}
