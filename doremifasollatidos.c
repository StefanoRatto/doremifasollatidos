#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>
#include <getopt.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>

#define VERSION "0.9.0"
#define MAX_RETRIES 3
#define INITIAL_BACKOFF_MS 100
#define MAX_BACKOFF_MS 1000
#define CURL_POOL_SIZE 100  // Size of the CURL handle pool per thread
#define MAX_TRACKED_STATUS_CODES 10

// Add safety checks for maximum values
#define MAX_THREAD_COUNT 10000
#define MIN_THREAD_COUNT 1
#define MAX_URL_LENGTH 2048
#define MAX_PAYLOAD_LENGTH (1024 * 1024)  // 1MB max payload
#define SAFE_CLEANUP_TIMEOUT_SEC 30

// Add thread state tracking
typedef enum {
    THREAD_UNINITIALIZED = 0,
    THREAD_RUNNING,
    THREAD_CLEANUP,
    THREAD_FINISHED
} ThreadState;

// Add thread tracking structure
typedef struct {
    pthread_t id;
    atomic_int state;
    bool initialized;
} ThreadInfo;

// Modify Configuration structure for safety
typedef struct {
    char *url;
    char *payload;
    int threads;
    long timeout_ms;
    char *request_method;
    size_t payload_len;
    struct curl_slist *headers;
    size_t url_len;  // Cache URL length
    ThreadInfo *thread_info;  // Track thread states
    bool initialization_complete;  // Track initialization state
} Config;

// Add this new structure after the existing structs
typedef struct {
    int code;
    atomic_uint_least64_t count;
} StatusCodeCount;

// Statistics structure with cache line padding to prevent false sharing
typedef struct {
    atomic_uint_least64_t request_count;
    atomic_uint_least64_t success_count;
    atomic_uint_least64_t failure_count;
    atomic_uint_least64_t throttle_count;
    atomic_uint_least64_t error_count;
    atomic_uint_least64_t total_latency_ms;
    StatusCodeCount status_codes[MAX_TRACKED_STATUS_CODES];
    atomic_int status_code_count;
    char padding[64];
} Statistics __attribute__((aligned(64)));

// Thread-local storage for CURL handles
typedef struct {
    CURL *handle;
    char error_buffer[CURL_ERROR_SIZE];
} CurlHandle;

// Global variables
static Config config = {0};
static Statistics *stats;  // Will be allocated with proper alignment
static volatile sig_atomic_t keep_running = 1;
static __thread CurlHandle *local_curl = NULL;  // Thread-local CURL handle

// Add new global variables for cleanup coordination
static volatile sig_atomic_t cleanup_in_progress = 0;

// Function prototypes
static void signal_handler(int signum);
static void *worker_thread(void *arg);
static void print_usage(void);
static void cleanup(void);
static void *progress_thread(void *arg);
static CURL *create_curl_handle(void);
static void init_curl_handle(CURL *curl);
static void update_status_code_stats(Statistics *stats, int status_code);
static void print_banner(void);
static void print_final_statistics(void);
static void *safe_malloc(size_t size);
static void *safe_aligned_alloc(size_t alignment, size_t size);
static char *safe_strdup(const char *str);
static bool validate_url(const char *url);
static bool validate_payload(const char *payload);
static bool validate_thread_count(int threads);
static void safe_cleanup_curl_handle(CURL *handle);
static void safe_cleanup_thread_info(void);
static bool initialize_thread_info(void);
static void *safe_calloc(size_t nmemb, size_t size);

// Microsecond precision timer
static inline uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// CURL write callback - discards received data efficiently
static size_t write_callback(void *ptr __attribute__((unused)), 
                           size_t size, 
                           size_t nmemb, 
                           void *userdata __attribute__((unused))) {
    return size * nmemb;
}

// Initialize a CURL handle with optimal settings
static void init_curl_handle(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_URL, config.url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);

    if (strcmp(config.request_method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, config.payload_len);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, config.payload);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, config.headers);
    }
}

// Create and initialize a new CURL handle
static CURL *create_curl_handle(void) {
    CURL *curl = curl_easy_init();
    if (curl) {
        init_curl_handle(curl);
    }
    return curl;
}

// Add this function before the worker_thread function
static void update_status_code_stats(Statistics *stats, int status_code) {
    if (!stats) return;
    
    int count = atomic_load(&stats->status_code_count);
    
    // Look for existing status code
    for (int i = 0; i < count && i < MAX_TRACKED_STATUS_CODES; i++) {
        if (stats->status_codes[i].code == status_code) {
            atomic_fetch_add(&stats->status_codes[i].count, 1);
            return;
        }
    }
    
    // Add new status code if space available
    if (count < MAX_TRACKED_STATUS_CODES) {
        int new_index = atomic_fetch_add(&stats->status_code_count, 1);
        if (new_index < MAX_TRACKED_STATUS_CODES) {
            stats->status_codes[new_index].code = status_code;
            atomic_store(&stats->status_codes[new_index].count, 1);
        }
    }
}

// Add new function to print final statistics
static void print_final_statistics(void) {
    printf("\n\nFinal Statistics:\n");
    uint64_t total_requests = atomic_load(&stats->request_count);
    uint64_t total_latency = atomic_load(&stats->total_latency_ms);
    
    printf("Total Requests: %" PRIu64 "\n", total_requests);
    printf("Average Latency: %" PRIu64 " ms\n", total_requests > 0 ? total_latency / total_requests : 0);
    printf("Successful: %" PRIu64 " (%.2f%%)\n", 
           atomic_load(&stats->success_count),
           total_requests > 0 ? (atomic_load(&stats->success_count) * 100.0) / total_requests : 0);
    printf("Failed: %" PRIu64 " (%.2f%%)\n", 
           atomic_load(&stats->failure_count),
           total_requests > 0 ? (atomic_load(&stats->failure_count) * 100.0) / total_requests : 0);
    printf("Throttled: %" PRIu64 "\n", atomic_load(&stats->throttle_count));
    printf("Errors: %" PRIu64 "\n", atomic_load(&stats->error_count));
    
    printf("\nResponse Code Distribution:\n");
    int code_count = atomic_load(&stats->status_code_count);
    for (int i = 0; i < code_count; i++) {
        uint64_t code_count = atomic_load(&stats->status_codes[i].count);
        printf("HTTP %d: %" PRIu64 " (%.2f%%)\n",
               stats->status_codes[i].code,
               code_count,
               total_requests > 0 ? (code_count * 100.0) / total_requests : 0);
    }
}

// Modify signal handler to print statistics before cleanup
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        // Only handle the first interrupt if cleanup hasn't started
        if (!cleanup_in_progress) {
            cleanup_in_progress = 1;
            keep_running = 0;
            printf("\nReceived interrupt signal. Press Ctrl+C again to force immediate exit.\n");
            printf("Gracefully shutting down (waiting for ongoing requests to complete)...\n");
            print_final_statistics();
        } else {
            // Second interrupt - force immediate exit
            printf("\nForcing immediate exit...\n");
            _exit(1);
        }
    }
}

// Add function to block signals in threads
static void block_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

// Add function to set up signal handling
static void setup_signal_handling(void) {
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    // Unblock signals in main thread
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

// Add validation functions
static bool validate_url(const char *url) {
    if (!url) return false;
    
    size_t len = strlen(url);
    if (len == 0 || len > MAX_URL_LENGTH) return false;
    
    // Basic URL validation
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    
    return true;
}

static bool validate_payload(const char *payload) {
    if (!payload) return true;  // Payload is optional
    
    size_t len = strlen(payload);
    if (len > MAX_PAYLOAD_LENGTH) return false;
    
    return true;
}

static bool validate_thread_count(int threads) {
    return threads >= MIN_THREAD_COUNT && threads <= MAX_THREAD_COUNT;
}

// Add thread info initialization
static bool initialize_thread_info(void) {
    if (!validate_thread_count(config.threads)) return false;
    
    config.thread_info = safe_calloc(config.threads, sizeof(ThreadInfo));
    if (!config.thread_info) return false;
    
    for (int i = 0; i < config.threads; i++) {
        atomic_store(&config.thread_info[i].state, THREAD_UNINITIALIZED);
        config.thread_info[i].initialized = false;
    }
    
    return true;
}

// Add safe memory allocation functions
static void *safe_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    if (size > SIZE_MAX / nmemb) return NULL;  // Check for multiplication overflow
    
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "Fatal: Failed to allocate %zu bytes\n", nmemb * size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

static void *safe_malloc(size_t size) {
    if (size == 0) return NULL;
    
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Fatal: Failed to allocate %zu bytes\n", size);
        exit(EXIT_FAILURE);
    }
    memset(ptr, 0, size);  // Always initialize allocated memory
    return ptr;
}

static void *safe_aligned_alloc(size_t alignment, size_t size) {
    if (size == 0 || alignment == 0) return NULL;
    if (size % alignment != 0) {
        size = (size + alignment - 1) & ~(alignment - 1);  // Round up to alignment
    }
    
    void *ptr = aligned_alloc(alignment, size);
    if (!ptr) {
        fprintf(stderr, "Fatal: Failed to allocate %zu bytes (aligned)\n", size);
        exit(EXIT_FAILURE);
    }
    memset(ptr, 0, size);  // Always initialize aligned memory
    return ptr;
}

static char *safe_strdup(const char *str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    if (len == 0) return NULL;
    
    char *dup = strdup(str);
    if (!dup) {
        fprintf(stderr, "Fatal: Failed to duplicate string\n");
        exit(EXIT_FAILURE);
    }
    return dup;
}

// Add safe cleanup functions
static void safe_cleanup_curl_handle(CURL *handle) {
    if (handle) {
        curl_easy_cleanup(handle);
    }
}

static void safe_cleanup_thread_info(void) {
    if (config.thread_info) {
        for (int i = 0; i < config.threads; i++) {
            if (config.thread_info[i].initialized) {
                pthread_join(config.thread_info[i].id, NULL);
            }
        }
        free(config.thread_info);
        config.thread_info = NULL;
    }
}

// Modify cleanup function for safety
static void cleanup(void) {
    if (!config.initialization_complete) return;
    
    safe_cleanup_thread_info();
    
    if (config.url) {
        free(config.url);
        config.url = NULL;
    }
    
    if (config.payload) {
        free(config.payload);
        config.payload = NULL;
    }
    
    if (config.request_method) {
        free(config.request_method);
        config.request_method = NULL;
    }
    
    if (config.headers) {
        curl_slist_free_all(config.headers);
        config.headers = NULL;
    }
    
    if (stats) {
        free(stats);
        stats = NULL;
    }
    
    curl_global_cleanup();
    config.initialization_complete = false;
}

// Modify worker thread for safety
static void *worker_thread(void *arg) {
    int thread_index = *(int *)arg;
    free(arg);  // Free the thread index argument
    
    if (thread_index < 0 || thread_index >= config.threads) {
        fprintf(stderr, "Error: Invalid thread index\n");
        return NULL;
    }
    
    atomic_store(&config.thread_info[thread_index].state, THREAD_RUNNING);
    
    // Initialize thread-local CURL handle with error checking
    if (!local_curl) {
        local_curl = safe_malloc(sizeof(CurlHandle));
        local_curl->handle = create_curl_handle();
        if (!local_curl->handle) {
            free(local_curl);
            atomic_store(&config.thread_info[thread_index].state, THREAD_FINISHED);
            fprintf(stderr, "Error: Failed to create CURL handle\n");
            return NULL;
        }
    }

    CURL *curl = local_curl->handle;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, local_curl->error_buffer);

    while (keep_running) {
        uint64_t start_time = get_timestamp_us();
        CURLcode res = curl_easy_perform(curl);
        uint64_t end_time = get_timestamp_us();
        
        // Check for integer overflow in latency calculation
        if (end_time < start_time) {
            fprintf(stderr, "Warning: Timer overflow detected\n");
            continue;
        }
        
        uint64_t latency_ms = (end_time - start_time) / 1000;
        if (latency_ms > UINT64_MAX / 1000) {
            fprintf(stderr, "Warning: Latency overflow detected\n");
            continue;
        }

        atomic_fetch_add(&stats->request_count, 1);
        atomic_fetch_add(&stats->total_latency_ms, latency_ms);

        if (res != CURLE_OK) {
            atomic_fetch_add(&stats->error_count, 1);
            usleep(INITIAL_BACKOFF_MS * 1000);
            continue;
        }

        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        update_status_code_stats(stats, response_code);

        switch (response_code) {
            case 200:
                atomic_fetch_add(&stats->success_count, 1);
                break;
            case 429: {
                atomic_fetch_add(&stats->throttle_count, 1);
                static __thread unsigned backoff_ms = INITIAL_BACKOFF_MS;
                usleep(backoff_ms * 1000);
                unsigned new_backoff = backoff_ms * 2;
                if (new_backoff < backoff_ms || new_backoff > MAX_BACKOFF_MS) {
                    backoff_ms = MAX_BACKOFF_MS;
                } else {
                    backoff_ms = new_backoff;
                }
                break;
            }
            default:
                atomic_fetch_add(&stats->failure_count, 1);
                break;
        }
    }

    // Cleanup thread-local resources
    if (local_curl) {
        safe_cleanup_curl_handle(local_curl->handle);
        free(local_curl);
        local_curl = NULL;
    }
    
    atomic_store(&config.thread_info[thread_index].state, THREAD_FINISHED);
    return NULL;
}

// Modify progress_thread to include signal blocking and respect cleanup state
static void *progress_thread(void *arg __attribute__((unused))) {
    // Block signals in progress thread
    block_signals();
    
    uint64_t last_requests = 0;
    time_t last_time = time(NULL);
    
    while (keep_running) {
        sleep(1);
        
        // Stop printing if cleanup has started
        if (cleanup_in_progress) {
            break;
        }
        
        time_t current_time = time(NULL);
        uint64_t current_requests = atomic_load(&stats->request_count);
        uint64_t rps = (current_requests - last_requests) / (current_time - last_time);
        uint64_t avg_latency = 0;
        
        if (current_requests > 0) {
            avg_latency = atomic_load(&stats->total_latency_ms) / current_requests;
        }

        printf("\rRPS: %" PRIu64 " | Avg Latency: %" PRIu64 "ms | Success: %" PRIu64 " | Failures: %" PRIu64 " | Throttled: %" PRIu64 " | Errors: %" PRIu64 " | Codes: ",
               rps, avg_latency,
               atomic_load(&stats->success_count),
               atomic_load(&stats->failure_count),
               atomic_load(&stats->throttle_count),
               atomic_load(&stats->error_count));

        // Print status code distribution
        int code_count = atomic_load(&stats->status_code_count);
        for (int i = 0; i < code_count; i++) {
            printf("%d:%lu ", 
                   stats->status_codes[i].code,
                   atomic_load(&stats->status_codes[i].count));
        }
        fflush(stdout);

        last_requests = current_requests;
        last_time = current_time;
    }
    return NULL;
}

// Print usage information
static void print_usage(void) {
    printf("Usage: doremifasollatidos [-g url] [-p url] [-d data] [-t threads] [-timeout ms]\n");
    printf("Options:\n");
    printf("  -g url      URL for GET request\n");
    printf("  -p url      URL for POST request\n");
    printf("  -d data     Data payload for POST request\n");
    printf("  -t threads  Number of concurrent threads (default: 500)\n");
    printf("  -timeout ms Request timeout in milliseconds (default: 10000)\n");
}

// Add print_banner implementation
static void print_banner(void) {
    printf("\033[1;35m"); // Bright pink color
    printf("\n");  // Add empty line before banner
    printf("·▄▄▄▄        ▄▄▄  ▄▄▄ .• ▌ ▄ ·. ▪  ·▄▄▄ ▄▄▄· .▄▄ ·       ▄▄▌  ▄▄▌   ▄▄▄· ▄▄▄▄▄▪  ·▄▄▄▄        .▄▄ · \n");
    printf("██▪ ██ ▪     ▀▄ █·▀▄.▀··██ ▐███▪██ ▐▄▄·▐█ ▀█ ▐█ ▀. ▪     ██•  ██•  ▐█ ▀█ •██  ██ ██▪ ██ ▪     ▐█ ▀. \n");
    printf("▐█· ▐█▌ ▄█▀▄ ▐▀▀▄ ▐▀▀▪▄▐█ ▌▐▌▐█·▐█·██▪ ▄█▀▀█ ▄▀▀▀█▄ ▄█▀▄ ██▪  ██▪  ▄█▀▀█  ▐█.▪▐█·▐█· ▐█▌ ▄█▀▄ ▄▀▀▀█▄\n");
    printf("██. ██ ▐█▌.▐▌▐█•█▌▐█▄▄▌██ ██▌▐█▌▐█▌██▌.▐█ ▪▐▌▐█▄▪▐█▐█▌.▐▌▐█▌▐▌▐█▌▐▌▐█ ▪▐▌ ▐█▌·▐█▌██. ██ ▐█▌.▐▌▐█▄▪▐█\n");
    printf("▀▀▀▀▀•  ▀█▄▀▪.▀  ▀ ▀▀▀ ▀▀  █▪▀▀▀▀▀▀▀▀▀  ▀  ▀  ▀▀▀▀  ▀█▄▀▪.▀▀▀ .▀▀▀  ▀  ▀  ▀▀▀ ▀▀▀▀▀▀▀▀•  ▀█▄▀▪ ▀▀▀▀ \n");
    printf("Version %s\n", VERSION);
    printf("\n");  // Add empty line after version
    printf("\033[0m"); // Reset color
}

// Modify main function for safety
int main(int argc, char *argv[]) {
    // Initialize with safe defaults
    memset(&config, 0, sizeof(Config));
    config.threads = 500;
    config.timeout_ms = 10000;
    
    // Display the banner
    print_banner();
    
    // Parse and validate command line arguments
    char *get_url = NULL;
    char *post_url = NULL;
    int opt;
    
    while ((opt = getopt(argc, argv, "g:p:d:t:h")) != -1) {
        switch (opt) {
            case 'g':
                if (!validate_url(optarg)) {
                    fprintf(stderr, "Error: Invalid URL format or length\n");
                    return 1;
                }
                get_url = optarg;
                break;
            case 'p':
                if (!validate_url(optarg)) {
                    fprintf(stderr, "Error: Invalid URL format or length\n");
                    return 1;
                }
                post_url = optarg;
                break;
            case 'd':
                if (!validate_payload(optarg)) {
                    fprintf(stderr, "Error: Invalid payload length\n");
                    return 1;
                }
                config.payload = safe_strdup(optarg);
                break;
            case 't':
                {
                    char *endptr;
                    long threads = strtol(optarg, &endptr, 10);
                    if (*endptr != '\0' || !validate_thread_count(threads)) {
                        fprintf(stderr, "Error: Invalid thread count (must be between %d and %d)\n",
                                MIN_THREAD_COUNT, MAX_THREAD_COUNT);
                        return 1;
                    }
                    config.threads = (int)threads;
                }
                break;
            case 'h':
                print_usage();
                return 0;
            default:
                print_usage();
                return 1;
        }
    }

    // Validate arguments
    if ((get_url && post_url) || (!get_url && !post_url)) {
        fprintf(stderr, "Error: Must specify either GET (-g) or POST (-p) request\n");
        print_usage();
        return 1;
    }

    if (post_url && !config.payload) {
        fprintf(stderr, "Error: POST request requires data payload (-d)\n");
        return 1;
    }

    // Initialize thread tracking
    if (!initialize_thread_info()) {
        fprintf(stderr, "Error: Failed to initialize thread tracking\n");
        return 1;
    }

    // Initialize statistics with proper alignment
    stats = safe_aligned_alloc(64, sizeof(Statistics));
    memset(stats, 0, sizeof(Statistics));

    // Set up configuration
    config.url = safe_strdup(get_url ? get_url : post_url);
    config.url_len = strlen(config.url);
    config.request_method = safe_strdup(get_url ? "GET" : "POST");

    if (config.payload) {
        config.payload_len = strlen(config.payload);
    }

    // Initialize CURL
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        fprintf(stderr, "Error: Failed to initialize CURL\n");
        cleanup();
        return 1;
    }

    // Initialize headers for POST requests
    if (strcmp(config.request_method, "POST") == 0) {
        config.headers = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
        if (!config.headers) {
            fprintf(stderr, "Error: Failed to create headers\n");
            cleanup();
            return 1;
        }
        config.headers = curl_slist_append(config.headers, "Connection: keep-alive");
        if (!config.headers) {
            fprintf(stderr, "Error: Failed to create headers\n");
            cleanup();
            return 1;
        }
    }

    // Set up signal handling
    setup_signal_handling();

    // Mark initialization as complete
    config.initialization_complete = true;

    // Create and start threads
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        fprintf(stderr, "Error: Failed to initialize thread attributes\n");
        cleanup();
        return 1;
    }

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) != 0) {
        fprintf(stderr, "Error: Failed to set thread attributes\n");
        pthread_attr_destroy(&attr);
        cleanup();
        return 1;
    }

    // Start progress thread
    pthread_t progress_thread_id;
    if (pthread_create(&progress_thread_id, &attr, progress_thread, NULL) != 0) {
        fprintf(stderr, "Error: Failed to create progress thread\n");
        pthread_attr_destroy(&attr);
        cleanup();
        return 1;
    }

    // Start worker threads
    for (int i = 0; i < config.threads; i++) {
        int *thread_index = safe_malloc(sizeof(int));
        *thread_index = i;
        
        if (pthread_create(&config.thread_info[i].id, &attr, worker_thread, thread_index) != 0) {
            fprintf(stderr, "Error: Failed to create worker thread %d\n", i);
            free(thread_index);
            
            // Cleanup already created threads
            keep_running = 0;
            for (int j = 0; j < i; j++) {
                pthread_join(config.thread_info[j].id, NULL);
            }
            pthread_join(progress_thread_id, NULL);
            pthread_attr_destroy(&attr);
            cleanup();
            return 1;
        }
        
        config.thread_info[i].initialized = true;
    }

    pthread_attr_destroy(&attr);

    // Wait for completion or interrupt
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += SAFE_CLEANUP_TIMEOUT_SEC;

    // Wait for threads to finish
    for (int i = 0; i < config.threads; i++) {
        if (config.thread_info[i].initialized) {
            int result = pthread_timedjoin_np(config.thread_info[i].id, NULL, &timeout);
            if (result == ETIMEDOUT) {
                fprintf(stderr, "Warning: Thread %d cleanup timed out\n", i);
            }
        }
    }

    // Wait for progress thread
    pthread_timedjoin_np(progress_thread_id, NULL, &timeout);

    // Final cleanup
    cleanup();
    return 0;
} 