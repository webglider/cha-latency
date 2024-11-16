#include <stdio.h>				// printf, etc
#include <stdint.h>				// standard integer types, e.g., uint32_t
#include <signal.h>				// for signal handler
#include <stdlib.h>				// exit() and EXIT_FAILURE
#include <string.h>				// strerror() function converts errno to a text string for printing
#include <fcntl.h>				// for open()
#include <errno.h>				// errno support
#include <assert.h>				// assert() function
#include <unistd.h>				// sysconf() function, sleep() function
#include <sys/mman.h>			// support for mmap() function
#include <math.h>				// for pow() function used in RAPL computations
#include <time.h>
#include <stdbool.h>
#include <sys/time.h>			// for gettimeofday
#include <sys/ipc.h>
#include <sys/shm.h>

#define SAMPLE_INTERVAL_SECS 1

#define CHA_MSR_PMON_BASE 0x0E00L
#define CHA_MSR_PMON_CTL_BASE 0x0E01L
#define CHA_MSR_PMON_FILTER0_BASE 0x0E05L
//#define CHA_MSR_PMON_FILTER1_BASE 0x0E06L // No FILTER1 on Icelake
#define CHA_MSR_PMON_STATUS_BASE 0x0E07L
#define CHA_MSR_PMON_CTR_BASE 0x0E08L
#define NUM_CHA_BOXES 18 // There are 32 CHA boxes in icelake server. After the first 18 boxes, the couter offsets change.
#define NUM_CHA_COUNTERS 4
#define CHA_FREQ 2400000000ULL


int TSC_ratio;

uint64_t cur_ctr_tsc[NUM_CHA_BOXES][NUM_CHA_COUNTERS], prev_ctr_tsc[NUM_CHA_BOXES][NUM_CHA_COUNTERS];
uint64_t cur_ctr_val[NUM_CHA_BOXES][NUM_CHA_COUNTERS], prev_ctr_val[NUM_CHA_BOXES][NUM_CHA_COUNTERS];


uint64_t prev_rdtsc = 0;
uint64_t cur_rdtsc = 0;

int msr_fd;


static inline __attribute__((always_inline)) unsigned long rdtsc()
{
   unsigned long a, d;

   __asm__ volatile("rdtsc" : "=a" (a), "=d" (d));

   return (a | (d << 32));
}


static inline __attribute__((always_inline)) unsigned long rdtscp()
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return (a | (d << 32));
}

extern inline __attribute__((always_inline)) int get_core_number()
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return ( c & 0xFFFUL );
}

static inline void sample_cha_ctr(int cha, int ctr) {
    uint32_t msr_num;
    uint64_t msr_val;
    int ret;

    msr_num = CHA_MSR_PMON_CTR_BASE + (0xE * cha) + ctr;
    ret = pread(msr_fd, &msr_val, sizeof(msr_val), msr_num);
    if (ret != sizeof(msr_val)) {
        perror("ERROR: failed to read MSR");
    }
    prev_ctr_val[cha][ctr] = cur_ctr_val[cha][ctr];
    cur_ctr_val[cha][ctr] = msr_val;
    prev_ctr_tsc[cha][ctr] = cur_ctr_tsc[cha][ctr];
    cur_ctr_tsc[cha][ctr] = rdtscp();
}

static void catch_function(int signal) {
	exit(0);
}

int main(int argc, char** argv){
    if (signal(SIGINT, catch_function) == SIG_ERR) {
		printf("An error occurred while setting the signal handler.\n");
		return EXIT_FAILURE;
	}

    int cpu = get_core_number();
    // fprintf(log_file,"Core no: %d\n",cpu);

    // Open MSR file for this core
    char filename[100];
    sprintf(filename, "/dev/cpu/%d/msr", cpu);
    msr_fd = open(filename, O_RDWR);
    if(msr_fd == -1) {
        printf("An error occurred while opening msr file.\n");
		return EXIT_FAILURE;
    }

    int cha, ctr, ret;
    uint32_t msr_num;
    uint64_t msr_val;

    // Get TSC frequency
    ret = pread(msr_fd, &msr_val, sizeof(msr_val), 0xCEL);
    TSC_ratio = (msr_val & 0x000000000000ff00L) >> 8;

    // Program CHA control registers
    for(cha = 0; cha < NUM_CHA_BOXES; cha++) {
        // Icelake offset multiplier is 0xE
        msr_num = CHA_MSR_PMON_FILTER0_BASE + (0xE * cha); // Filter0
        msr_val = 0x00000000; // default; no filtering
        ret = pwrite(msr_fd, &msr_val,sizeof(msr_val),msr_num);
        if (ret != 8) {
        printf("wrmsr FILTER0 failed for cha: %d\n", cha);
            perror("wrmsr FILTER0 failed");
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0xE * cha) + 0; // counter 0
        msr_val = (cha%2==0)?(0x00c8168600400136):(0x00c8170600400136); // TOR Occupancy, DRd, Miss, local/remote on even/odd CHA boxes
        ret = pwrite(msr_fd,&msr_val,sizeof(msr_val),msr_num);
        if (ret != 8) {
            perror("wrmsr COUNTER0 failed");
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0xE * cha) + 1; // counter 1
        msr_val = (cha%2==0)?(0x00c8168600400135):(0x00c8170600400135); // TOR Inserts, DRd, Miss, local/remote on even/odd CHA boxes
        ret = pwrite(msr_fd,&msr_val,sizeof(msr_val),msr_num);
        if (ret != 8) {
            perror("wrmsr COUNTER1 failed");
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0xE * cha) + 2; // counter 2
        msr_val = 0x400000; // CLOCKTICKS
        ret = pwrite(msr_fd,&msr_val,sizeof(msr_val),msr_num);
        if (ret != 8) {
            perror("wrmsr COUNTER2 failed");
        }
    }

    // Sample CHA counters
    prev_rdtsc = rdtscp();
    sample_cha_ctr(0, 0);
    sample_cha_ctr(0, 1);
    sample_cha_ctr(1, 0);
    sample_cha_ctr(1, 1);
    double cur_occ, cur_rate, cur_lat, ns_per_clk;
    ns_per_clk = ((double)1e9)/((double)CHA_FREQ);
    printf("Local Remote\n");
    while(1) {
        cur_rdtsc = rdtscp();
        if(cur_rdtsc > prev_rdtsc + SAMPLE_INTERVAL_SECS*TSC_ratio*100*1e6) {
            sample_cha_ctr(0, 0); // CHA0 occupancy
            sample_cha_ctr(0, 1); // CHA0 inserts
            sample_cha_ctr(1, 0);
            sample_cha_ctr(1, 1);
            
            // Local
            cur_occ = (double)(cur_ctr_val[0][0] - prev_ctr_val[0][0]);
            cur_rate = (double)(cur_ctr_val[0][1] - prev_ctr_val[0][1]);
            cur_lat = (cur_occ/cur_rate)*ns_per_clk;
            printf("%lf", cur_lat);

            // Remote
            cur_occ = (double)(cur_ctr_val[1][0] - prev_ctr_val[1][0]);
            cur_rate = (double)(cur_ctr_val[1][1] - prev_ctr_val[1][1]);
            cur_lat = (cur_occ/cur_rate)*ns_per_clk;
            printf(" %lf", cur_lat);

            printf("\n");
            fflush(stdout);
            prev_rdtsc = cur_rdtsc;
        }
    }

    return 0;
}
