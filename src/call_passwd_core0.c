#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

#define PASSWD_PATH "/usr/bin/passwd"
#define PASSWD_DELAY 50000

int main(){
    uint64_t out;
    unsigned char *out_ptr = (unsigned char *)&out;

    while (1) {
        execv(PASSWD_PATH);
        usleep(PASSWD_DELAY);
    }
}