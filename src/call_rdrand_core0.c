#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>


#define RAND_DELAY 500000

int main(){
    uint64_t out;
    unsigned char *out_ptr = (unsigned char *)&out;

    while (1) {
        asm volatile("rdrand %%rax\n":"=a"(out):);
        printf("[#] Got: %02x%02x%02x%02x\n", out_ptr[0], out_ptr[1], out_ptr[2], out_ptr[3]);
        usleep(RAND_DELAY);
    }
}