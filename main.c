#define _DEFAULT_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <asm-generic/errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>

int WIDTH  = 256;
int HEIGHT = 32;

// -1 means center of the screen
int X = -1;
int Y = -1;

int SECONDS_DELAY = 2;


char ICON_TEXT[32] = "";


// Defines
#define CPU_FILE_PATH "/proc/stat"
#define CPU_MAX_VALS 10
#define SEC_TO_WAIT 2
#define WAIT_UNTIL_UPDATE 25000 //ns

// The delay between refreshing cpu values (ns)
unsigned long REFRESH_SPEED = 1000000;

// CPU Calculation
typedef struct CpuInfo CpuInfo;
struct CpuInfo {
  uint64_t idle;
  uint64_t total;
  uint32_t perc;
};

typedef struct CpuCalcRefreshInfo CpuCalcRefreshInfo;
struct CpuCalcRefreshInfo {
  pthread_t thread;            // Thread that calculates cpu percent
  pthread_mutex_t wait_mutex;  // Interval wait mutex
  pthread_cond_t wait_cond;    // Interval wait condition variable
  CpuInfo *cpu_info;
  size_t num_cpus;
};

typedef size_t (*GraphGetValueFunc)();

typedef struct NeuroGraph NeuroGraph;
struct NeuroGraph {
    size_t wBar;    //Bar width
    float hScale; //How much do we need to Scale the bars?
    size_t nBars;
    uint32_t *data;
    FILE *stream;
    
    //Colors
    char *normalColor;
    char *mediumColor;
    char *highColor;
    
    //treshholds
    uint32_t mediumTresh;
    uint32_t highTresh;
    
    GraphGetValueFunc getValue;
};

//GLOBALS
static CpuCalcRefreshInfo cpu_calc_refresh_info_;
static bool cpu_calc_stop_refresh_cond_ = false;



void get_cpu_usage_from_core(float *cpu, int core);

size_t get_num_cpus(const char *file);

// Note: Returns true if it has timed out or false when the condition variable has been notified
static bool cond_timedwait(time_t seconds, bool *cond_stop, pthread_mutex_t *mutex, pthread_cond_t *cond_var) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += seconds;
  bool timedout = false;
  pthread_mutex_lock(mutex);
  while (!*cond_stop)
    if (ETIMEDOUT == pthread_cond_timedwait(cond_var, mutex, &deadline)) {
      timedout = true;
      break;
    }
  pthread_mutex_unlock(mutex);
  return timedout;
}

// CPU Calculation (Thread 1)
static bool cpu_calc_refresh_timedwait(time_t seconds) {
  return cond_timedwait(seconds, &cpu_calc_stop_refresh_cond_, &cpu_calc_refresh_info_.wait_mutex, &cpu_calc_refresh_info_.wait_cond);
}

static void get_perc_info(CpuInfo *cpu_info, uint64_t *cpu_vals, uint64_t prev_idle, uint64_t prev_total) {
  assert(cpu_info);
  assert(cpu_vals);
  cpu_info->idle = cpu_vals[ 3 ];
  cpu_info->total = 0L;
  for (size_t i = 0U; i < CPU_MAX_VALS; ++i)
    cpu_info->total += cpu_vals[ i ];
  const uint64_t diff_idle = cpu_info->idle - prev_idle;
  const uint64_t diff_total = cpu_info->total - prev_total;
  cpu_info->perc = (100 * (diff_total - diff_idle)) / diff_total;
}

static void refresh_cpu_calc(const char *file, size_t ncpus) {
  assert(file);
  uint64_t cpus_file_info[ ncpus ][ CPU_MAX_VALS ];
  uint64_t prev_idle[ ncpus ], prev_total[ ncpus ];
  memset(prev_idle, 0, sizeof(prev_idle));
  memset(prev_total, 0, sizeof(prev_total));

  while (true) {
    // Open the file
    FILE *const fd = fopen(file, "r");
    if (!fd)
      return;

    // Do the percent calculation
    char buf[ 256 ];
    for (size_t i = 0U; i < ncpus; ++i) {
      fgets(buf, sizeof(buf), fd);
      if (EOF == sscanf(buf + 5, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
          " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
          cpus_file_info[ i ] + 0, cpus_file_info[ i ] + 1,
          cpus_file_info[ i ] + 2, cpus_file_info[ i ] + 3,
          cpus_file_info[ i ] + 4, cpus_file_info[ i ] + 5,
          cpus_file_info[ i ] + 6, cpus_file_info[ i ] + 7,
          cpus_file_info[ i ] + 8, cpus_file_info[ i ] + 9))
        return;
      get_perc_info(cpu_calc_refresh_info_.cpu_info + i, cpus_file_info[ i ], prev_idle[ i ], prev_total[ i ]);
      prev_idle[ i ] = cpu_calc_refresh_info_.cpu_info[ i ].idle;
      prev_total[ i ] = cpu_calc_refresh_info_.cpu_info[ i ].total;
    }

    // Close the file
    fclose(fd);

    // Wait 1 second or break if the conditional variable has been signaled
    if (!cpu_calc_refresh_timedwait(SEC_TO_WAIT))
      break;
  }
}

static void *refresh_cpu_calc_thread(void *args) {
  (void)args;
  refresh_cpu_calc(CPU_FILE_PATH, cpu_calc_refresh_info_.num_cpus);
  pthread_exit(NULL);
}

static bool init_cpu_calc_thread(void) {
  // Init mutex and cond
  pthread_mutex_init(&cpu_calc_refresh_info_.wait_mutex, NULL);
  pthread_cond_init(&cpu_calc_refresh_info_.wait_cond, NULL);

  // Create thread
  return 0 == pthread_create(&cpu_calc_refresh_info_.thread, NULL, refresh_cpu_calc_thread, NULL);
}

static void stop_cpu_calc_thread(void) {
  // Stop calc thread
  cpu_calc_stop_refresh_cond_ = true;
  pthread_cond_broadcast(&cpu_calc_refresh_info_.wait_cond);

  // Join thread
  void *status;
  if (pthread_join(cpu_calc_refresh_info_.thread, &status))  // Wait
    perror("stop_cpu_calc_thread - Could not join thread");

  // Destroy cond and mutex
  pthread_cond_destroy(&cpu_calc_refresh_info_.wait_cond);
  pthread_mutex_destroy(&cpu_calc_refresh_info_.wait_mutex);
}

static bool init_cpu_calc_refresh_info(void) {
  cpu_calc_refresh_info_.num_cpus = get_num_cpus(CPU_FILE_PATH);
  if (cpu_calc_refresh_info_.num_cpus <= 0)
    return false;
  cpu_calc_refresh_info_.cpu_info = (CpuInfo *)calloc(cpu_calc_refresh_info_.num_cpus, sizeof(CpuInfo));
  return cpu_calc_refresh_info_.cpu_info != NULL;
}

static void stop_cpu_calc_refresh_info(void) {
  free(cpu_calc_refresh_info_.cpu_info);
  cpu_calc_refresh_info_.cpu_info = NULL;
}


size_t get_num_cpus(const char *file) 
{
  FILE *const fd = fopen(file, "r");
  if (!fd)
    return 0;
  size_t i = 0U;
  char buf[ 256 ];
  while (fgets(buf, sizeof(buf), fd)) {
    if (strncmp(buf, "cpu", 3) != 0)
      break;
    ++i;
  }
  fclose(fd);
  return i;
}


size_t getCPUValue()
{
    return cpu_calc_refresh_info_.cpu_info[0].perc;
}
void init_NeuroGraph(NeuroGraph *g, const char *file, int w, int h, int nBars, GraphGetValueFunc func)
{
    g->data = malloc(sizeof(uint32_t) * nBars);

    for(int i = 0; i < nBars; i++)
        g->data[i] = 0;
    
    char *command = malloc(sizeof(char) * 256);
    sprintf(command, "%s -w %u -h %u\0", file, w, h);
    g->stream = popen(command, "w");
    free(command);
    
    int tmp = (nBars > w) ? w : nBars;
    
    g->nBars = tmp;
    g->wBar = w/tmp;
    g->hScale = (float)h/(float)100;

    g->normalColor = (char *)malloc(10);
    g->mediumColor = (char *)malloc(10);
    g->highColor   = (char *)malloc(10);
    
    g->mediumTresh = 50;
    g->highTresh   = 90;
    
    strcpy(g->mediumColor, "orange"); 
    strcpy(g->highColor, "red"); 

    g->getValue = func;
}
void graph(NeuroGraph *g)
{
    char *rect = (char *)malloc(30);
    
    char *string = (char *)malloc(sizeof(char) * 30 * g->nBars);
    char *save = string;
    int size = 0;
            
    while(1){
        uint32_t cpu = g->getValue();
        
        {
            //Fill the data struc
            for(int i = 1; i < g->nBars; i++){
                g->data[i-1] = g->data[i];
            }
            g->data[g->nBars-1] = cpu;
            
            //Build the dzen2 string
            for(int i = 0; i < g->nBars; i++)
            {
                sprintf(rect, "%s^fg(%s)^r(%ux%u)", 
                       (i == 0) ? "^pa(1;0)" : "",
                       (g->data[i] >= g->mediumTresh) ? ((g->data[i] >= g->highTresh) ? g->highColor : g->mediumColor) : "",
                       (g->wBar), 
                       (int)ceil((float)g->data[i] * g->hScale)
                ); 

                size = strlen(rect);
                sprintf(string, "%s", rect);
                string += size;
            }
            sprintf(string, "%s", "\n");
            //printf("%s", save);
            
            fprintf(g->stream, save);
            fflush(g->stream);
            
            string = save;
        }

        usleep(REFRESH_SPEED);
    }
    free(save);

    fflush(NULL);
    pclose(g->stream);
    return;
}

int main(int argc, char *argv[]){
    if (!init_cpu_calc_refresh_info()){
      printf("Could Not Init Cpu Calc refresh info\n");
      return 0;
    }
    if (!init_cpu_calc_thread()){
      printf("Could not init cpu calc thread\n");
      return 0;
    }
   
    //Init the data Struct
    NeuroGraph g;
    init_NeuroGraph(&g, "dzen2 -ta l", 150, 20, 250, getCPUValue);
    graph(&g); //TODO hier pthread starten
}

