# DoReMiFaSolLaTiDOS

A high-performance, concurrent HTTP load testing tool written in C. This tool allows you to perform load testing on web servers by sending multiple concurrent GET or POST requests with optimized performance and detailed metrics.

## Features

### Core Features
- Concurrent request handling with configurable number of threads
- Support for both GET and POST requests
- Real-time performance metrics with colored output
- Configurable request timeout
- Graceful shutdown with double Ctrl+C protection
- Detailed success/failure statistics
- Connection pooling for better performance
- Proper error handling and reporting

### Performance Optimizations
- Thread-local storage for CURL handles
- Connection pooling and reuse
- TCP keep-alive support
- Cache-aligned data structures
- Exponential backoff for error handling
- Efficient memory management
- Zero-copy response handling

### Advanced Metrics
- Real-time Requests Per Second (RPS)
- Average latency tracking
- Success/failure percentages
- Throttling detection
- Detailed error reporting
- HTTP status code distribution
- Comprehensive final statistics

## Requirements

- C compiler (GCC or Clang)
- libcurl development package
- POSIX-compliant system (Linux, macOS, BSD)

### Installing Dependencies

On Debian/Ubuntu:
```bash
sudo apt-get install build-essential libcurl4-openssl-dev
```

On macOS:
```bash
brew install curl
```

## Building

### Basic Build
```bash
gcc -o doremifasollatidos doremifasollatidos.c -lcurl -pthread
```

### Optimized Build (Recommended)
```bash
gcc -O3 -march=native -flto -pthread -o doremifasollatidos doremifasollatidos.c -lcurl
```

Build options explained:
- `-O3`: Highest level of optimization
- `-march=native`: Optimize for the current CPU architecture
- `-flto`: Enable Link Time Optimization
- `-pthread`: Enable POSIX threads support

## Usage

### Command Line Options

- `-g` : URL for GET request (e.g., `-g 'http://example.com'`)
- `-p` : URL for POST request (e.g., `-p 'http://example.com'`)
- `-d` : Data payload for POST request (required when using POST)
- `-t` : Number of concurrent threads (default: 500)
- `-timeout` : Request timeout in milliseconds (default: 10000)

### Examples

#### GET Request
```bash
# Basic GET request with 100 concurrent threads
./doremifasollatidos -g "http://example.com" -t 100

# GET request with custom timeout
./doremifasollatidos -g "http://example.com" -t 100 -timeout 5000
```

#### POST Request
```bash
# POST request with form data
./doremifasollatidos -p "http://example.com" -d "key1=value1&key2=value2" -t 100

# POST request with custom timeout and threads
./doremifasollatidos -p "http://example.com" -d "data=test" -t 50 -timeout 3000
```

## Output and Metrics

The tool provides real-time statistics and a comprehensive final report:

### Real-time Metrics
```
RPS: 1500 | Avg Latency: 45ms | Success: 1450 | Failures: 20 | Throttled: 25 | Errors: 5 | Codes: 200:1450 429:25 500:20
```

### Final Statistics
```
Final Statistics:
Total Requests: 15000
Average Latency: 48 ms
Successful: 14500 (96.67%)
Failed: 450 (3.00%)
Throttled: 25
Errors: 25

Response Code Distribution:
HTTP 200: 14500 (96.67%)
HTTP 429: 25 (0.17%)
HTTP 500: 475 (3.17%)
```

## Interrupt Handling

The tool implements a safe shutdown mechanism:

1. First Ctrl+C:
   - Displays warning message
   - Initiates graceful shutdown
   - Waits for ongoing requests to complete
   - Prints final statistics

2. Second Ctrl+C:
   - Forces immediate program termination
   - Use only if graceful shutdown is taking too long

This ensures both safe operation and user control over the shutdown process.

## Performance Features

### Connection Management
- Efficient connection pooling
- TCP keep-alive support
- Connection reuse
- Optimized curl settings

### Memory Optimization
- Cache-aligned data structures
- Thread-local storage
- Zero-copy response handling
- Efficient memory allocation

### Concurrency
- Thread-per-connection model
- Lock-free statistics collection
- Atomic operations for counters
- Thread-safe resource management

### Error Handling
- Exponential backoff for errors
- Rate limiting detection
- Graceful error recovery
- Detailed error reporting

## Performance Considerations

- Each thread maintains its own connection pool
- Response bodies are efficiently discarded
- Atomic operations ensure thread-safe statistics
- Backoff mechanisms prevent server overload
- Memory alignment optimizes cache usage
- Graceful shutdown with timeout protection

## Limitations

- Only supports basic GET and POST requests
- POST data must be URL-encoded
- No support for custom headers (except Content-Type for POST)
- No support for HTTPS client certificates
- Maximum timeout is system dependent

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Performance Tips

1. **Thread Count**: Start with a thread count equal to 2x the number of CPU cores and adjust based on your needs.
2. **Timeout Values**: Set timeouts appropriate for your application (default 10s).
3. **System Limits**: You may need to adjust system limits for maximum performance:
   ```bash
   # Increase system limits for high concurrency
   ulimit -n 65535  # Increase open file limit
   ```

## Troubleshooting

### Common Issues

1. **Too many open files**
   ```bash
   sudo sysctl -w fs.file-max=65535
   ulimit -n 65535
   ```

2. **Connection refused**
   - Check if the target server is running
   - Verify firewall settings
   - Ensure the URL is correct

3. **High latency**
   - Reduce thread count
   - Check network conditions
   - Monitor system resources

4. **Unexpected termination**
   - First Ctrl+C initiates graceful shutdown
   - Wait for ongoing requests to complete
   - Use second Ctrl+C only if necessary

## Contributing

Feel free to submit issues, fork the repository, and create pull requests for any improvements.

# Licensing

The tool is licensed under the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.en.html).

# Legal disclaimer

Usage of this tool to interact with targets without prior mutual consent is illegal. It's the end user's responsibility to obey all applicable local, state and federal laws. Developers assume no liability and are not responsible for any misuse or damage caused by this program. Only use for educational purposes.regulations. 
