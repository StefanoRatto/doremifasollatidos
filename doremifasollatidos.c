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

#define VERSION "1.0.0"
#define MAX_RETRIES 3
#define INITIAL_BACKOFF_MS 100
#define MAX_BACKOFF_MS 1000
#define CURL_POOL_SIZE 100  // Size of the CURL handle pool per thread
#define MAX_TRACKED_STATUS_CODES 10

// Configuration structure
typedef struct {
    char *url;
    char *payload;
    int threads;
    long timeout_ms;
    char *request_method;
    size_t payload_len;  // Cache the payload length
    struct curl_slist *headers;  // Reusable headers
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
static pthread_mutex_t cleanup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cleanup_cond = PTHREAD_COND_INITIALIZER;
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

// Microsecond precision timer
static inline uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// CURL write callback - discards received data efficiently
static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
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
    int count = atomic_load(&stats->status_code_count);
    
    // Look for existing status code
    for (int i = 0; i < count; i++) {
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

// Modify worker_thread to include signal blocking
static void *worker_thread(void *arg) {
    // Block signals in worker threads
    block_signals();
    
    // Initialize thread-local CURL handle
    if (!local_curl) {
        local_curl = malloc(sizeof(CurlHandle));
        local_curl->handle = create_curl_handle();
        if (!local_curl->handle) {
            free(local_curl);
            return NULL;
        }
    }

    CURL *curl = local_curl->handle;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, local_curl->error_buffer);

    while (keep_running) {
        uint64_t start_time = get_timestamp_us();
        CURLcode res = curl_easy_perform(curl);
        uint64_t end_time = get_timestamp_us();
        uint64_t latency_ms = (end_time - start_time) / 1000;

        atomic_fetch_add(&stats->request_count, 1);
        atomic_fetch_add(&stats->total_latency_ms, latency_ms);

        if (res != CURLE_OK) {
            atomic_fetch_add(&stats->error_count, 1);
            // Implement exponential backoff for errors
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
                // Implement rate limiting backoff
                static __thread unsigned backoff_ms = INITIAL_BACKOFF_MS;
                usleep(backoff_ms * 1000);
                backoff_ms = (backoff_ms * 2 > MAX_BACKOFF_MS) ? MAX_BACKOFF_MS : backoff_ms * 2;
                break;
            }
            default:
                atomic_fetch_add(&stats->failure_count, 1);
                break;
        }
    }

    // Cleanup thread-local resources
    if (local_curl) {
        curl_easy_cleanup(local_curl->handle);
        free(local_curl);
    }
    return NULL;
}

// Modify progress_thread to include signal blocking and respect cleanup state
static void *progress_thread(void *arg) {
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

// Cleanup resources
static void cleanup(void) {
    free(config.url);
    free(config.payload);
    free(config.request_method);
    curl_global_cleanup();
}

// Add new cleanup function for graceful shutdown
static void perform_cleanup(pthread_t *threads, int thread_count, pthread_t progress_thread_id) {
    // Wait for all worker threads with a timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30; // 30 second timeout for graceful shutdown

    for (int i = 0; i < thread_count; i++) {
        pthread_timedjoin_np(threads[i], NULL, &timeout);
    }
    
    // Wait for progress thread
    pthread_timedjoin_np(progress_thread_id, NULL, &timeout);
    
    // Clean up resources
    free(threads);
    free(stats);
    if (config.headers) {
        curl_slist_free_all(config.headers);
    }
    cleanup();
}

// Add the banner function near the top with other utility functions
static void print_banner(void) {
    const char *banner =
        "\033[1;35m"  // Bright pink color
        "                                                                                                                                                     \n"
        "        ,,                                             ,,      ,...                         ,,    ,,                  ,,        ,,                   \n"
        "      `7MM                                             db    .d\' \"\"                       `7MM  `7MM           mm     db      `7MM                   \n"
        "        MM                                                   dM`                            MM    MM           MM               MM                   \n"
        "   ,M\"\"bMM  ,pW\"Wq.`7Mb,od8 .gP\"Ya `7MMpMMMb.pMMMb.  `7MM   mMMmm ,6\"Yb.  ,pP\"Ybd  ,pW\"Wq.  MM    MM   ,6\"Yb.mmMMmm `7MM   ,M\"\"bMM  ,pW\"Wq.  ,pP\"Ybd \n"
        " ,AP    MM 6W\'   `Wb MM\' \"\',M\'   Yb  MM    MM    MM    MM    MM  8)   MM  8I   `\" 6W\'   `Wb MM    MM  8)   MM  MM     MM ,AP    MM 6W\'   `Wb 8I   `\" \n"
        " 8MI    MM 8M     M8 MM    8M\"\"\"\"\"\"  MM    MM    MM    MM    MM   ,pm9MM  `YMMMa. 8M     M8 MM    MM   ,pm9MM  MM     MM 8MI    MM 8M     M8 `YMMMa. \n"
        " `Mb    MM YA.   ,A9 MM    YM.    ,  MM    MM    MM    MM    MM  8M   MM  L.   I8 YA.   ,A9 MM    MM  8M   MM  MM     MM `Mb    MM YA.   ,A9 L.   I8 \n"
        "  `Wbmd\"MML.`Ybmd9\'.JMML.   `Mbmmd\'.JMML  JMML  JMML..JMML..JMML.`Moo9^Yo.M9mmmP\'  `Ybmd9\'.JMML..JMML.`Moo9^Yo.`Mbmo.JMML.`Wbmd\"MML.`Ybmd9\'  M9mmmP\' \n"
        "\033[0m"  // Reset color
        "\n";

    printf("%s", banner);
}

// Main function with improved resource management
int main(int argc, char *argv[]) {
    // Display the banner
    print_banner();

    // Allocate statistics with proper alignment
    stats = aligned_alloc(64, sizeof(Statistics));
    memset(stats, 0, sizeof(Statistics));

    // Initialize default values
    config.threads = 500;
    config.timeout_ms = 10000;
    char *get_url = NULL;
    char *post_url = NULL;
    int opt;

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "g:p:d:t:h")) != -1) {
        switch (opt) {
            case 'g':
                get_url = optarg;
                break;
            case 'p':
                post_url = optarg;
                break;
            case 'd':
                config.payload = strdup(optarg);
                break;
            case 't':
                config.threads = atoi(optarg);
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

    // Set up configuration
    config.url = strdup(get_url ? get_url : post_url);
    config.request_method = strdup(get_url ? "GET" : "POST");

    // Pre-calculate payload length
    if (config.payload) {
        config.payload_len = strlen(config.payload);
    }

    // Initialize reusable headers
    if (strcmp(config.request_method, "POST") == 0) {
        config.headers = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
        config.headers = curl_slist_append(config.headers, "Connection: keep-alive");
    }

    // Initialize CURL with thread safety
    curl_global_init(CURL_GLOBAL_ALL);

    // Set up signal handling before creating threads
    setup_signal_handling();

    // Allocate and create threads
    pthread_t *threads = aligned_alloc(64, config.threads * sizeof(pthread_t));
    pthread_t progress_thread_id;

    // Set thread attributes for performance
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    // Start progress thread
    pthread_create(&progress_thread_id, &attr, progress_thread, NULL);

    // Start worker threads
    for (int i = 0; i < config.threads; i++) {
        pthread_create(&threads[i], &attr, worker_thread, NULL);
    }

    pthread_attr_destroy(&attr);

    // Wait for either completion or interrupt
    perform_cleanup(threads, config.threads, progress_thread_id);

    // Print final statistics only if we haven't already printed them during cleanup
    if (!cleanup_in_progress) {
        print_final_statistics();
    }

    return 0;
} 