# DoReMiFaSolLaTiDOS

A high-performance, memory-safe concurrent HTTP load testing tool written in C. This tool allows you to perform load testing on web servers by sending multiple concurrent GET or POST requests with optimized performance, comprehensive safety checks, and detailed metrics.

## Features

### Core Features
- Concurrent request handling with configurable number of threads
- Support for both GET and POST requests
- Real-time performance metrics with colored output
- Configurable request timeout
- Graceful shutdown with double Ctrl+C protection
- Detailed success/failure statistics
- Connection pooling for better performance
- Comprehensive memory safety checks
- Thread state tracking and safe cleanup

### Safety Features
- Input validation for all parameters
- Memory bounds checking
- Integer overflow protection
- Thread state tracking
- Safe resource cleanup
- Timeout protection
- Error recovery
- NULL pointer protection
- Buffer overflow prevention

### Memory Safety
- Safe memory allocation with overflow checks
- Zero-initialization of all allocated memory
- Proper alignment handling
- Resource cleanup coordination
- Thread-safe operations
- Memory leak prevention
- Double-free prevention
- Safe string handling

### Thread Safety
- Thread state tracking
- Atomic operations
- Safe thread cleanup
- Resource cleanup coordination
- Thread initialization verification
- Thread join timeout protection
- Thread-local storage safety
- Signal handling safety

### Input Validation
- URL format and length validation (max 2048 bytes)
- Payload size limits (max 1MB)
- Thread count bounds (1-10000 threads)
- Command-line argument validation
- HTTP response validation
- Timeout validation
- Resource limit checks
- State transition validation

### Performance Optimizations
- Thread-local storage for CURL handles
- Connection pooling and reuse
- TCP keep-alive support
- Cache-aligned data structures
- Exponential backoff for error handling
- Efficient memory management
- Zero-copy response handling
- Atomic counters

### Advanced Metrics
- Real-time Requests Per Second (RPS)
- Average latency tracking
- Success/failure percentages
- Throttling detection
- Detailed error reporting
- HTTP status code distribution
- Comprehensive final statistics
- Thread state monitoring

## Requirements

- C compiler (GCC or Clang)
- libcurl development package
- POSIX-compliant system (Linux, macOS, BSD)
- Sufficient system resources for thread limits

### System Requirements

- Memory: Depends on thread count (approximately 1MB per thread)
- CPU: Multi-core recommended
- File descriptors: At least (thread_count * 2) available
- Virtual memory: At least 2GB recommended

### Resource Limits

- Maximum threads: 10000
- Minimum threads: 1
- Maximum URL length: 2048 bytes
- Maximum payload size: 1MB
- Default timeout: 10 seconds
- Cleanup timeout: 30 seconds

## Building

### Basic Build
```bash
gcc -o doremifasollatidos doremifasollatidos.c -lcurl -pthread
```

### Optimized Build (Recommended)
```bash
gcc -O3 -march=native -flto -pthread -Wall -Wextra -Werror -o doremifasollatidos doremifasollatidos.c -lcurl
```

Build options explained:
- `-O3`: Highest level of optimization
- `-march=native`: Optimize for current CPU
- `-flto`: Enable Link Time Optimization
- `-pthread`: Enable POSIX threads support
- `-Wall -Wextra -Werror`: Enable strict warning checks

## Usage

### Command Line Options

- `-g` : URL for GET request (e.g., `-g 'http://example.com'`)
- `-p` : URL for POST request (e.g., `-p 'http://example.com'`)
- `-d` : Data payload for POST request (required when using POST)
- `-t` : Number of concurrent threads (default: 500, range: 1-10000)
- `-timeout` : Request timeout in milliseconds (default: 10000)

### Safety Considerations

1. **Thread Count**: Choose based on system resources:
   - Start with thread count = 2 * CPU cores
   - Monitor system load
   - Stay within system limits
   - Consider network capacity

2. **Memory Usage**:
   - Each thread uses approximately 1MB
   - Monitor system memory
   - Consider payload size
   - Watch for system limits

3. **Network Resources**:
   - Check file descriptor limits
   - Monitor network capacity
   - Consider target server limits
   - Watch for connection limits

4. **Cleanup Behavior**:
   - First Ctrl+C: Graceful shutdown
   - Second Ctrl+C: Immediate exit
   - 30-second cleanup timeout
   - Resource cleanup verification

### Error Handling

The tool implements comprehensive error handling:

1. **Memory Errors**:
   - Allocation failures
   - Overflow protection
   - NULL pointer checks
   - Double-free prevention

2. **Thread Errors**:
   - Creation failures
   - Join timeouts
   - State transitions
   - Resource cleanup

3. **Network Errors**:
   - Connection failures
   - Timeout handling
   - Protocol errors
   - Rate limiting

4. **Input Validation**:
   - URL format checks
   - Size limits
   - Thread bounds
   - Payload validation

## Troubleshooting

### Common Issues

1. **Resource Limits**
   ```bash
   # Increase system limits
   ulimit -n 65535  # File descriptors
   ulimit -u 65535  # Max user processes
   ```

2. **Memory Issues**
   - Reduce thread count
   - Monitor system memory
   - Check for leaks
   - Verify limits

3. **Thread Issues**
   - Check system limits
   - Reduce thread count
   - Monitor CPU usage
   - Check core count

4. **Network Issues**
   - Verify connectivity
   - Check DNS resolution
   - Monitor bandwidth
   - Check firewalls

## Performance Tips

1. **Thread Count Optimization**:
   - Start with 2 * CPU cores
   - Increase gradually
   - Monitor system load
   - Stay within limits

2. **Memory Optimization**:
   - Monitor RSS usage
   - Check virtual memory
   - Watch swap usage
   - Adjust thread count

3. **Network Optimization**:
   - Use keep-alive
   - Monitor bandwidth
   - Check latency
   - Adjust timeouts

4. **System Tuning**:
   ```bash
   # Optimize system settings
   sysctl -w net.ipv4.tcp_fin_timeout=30
   sysctl -w net.ipv4.tcp_max_tw_buckets=65536
   sysctl -w net.ipv4.tcp_max_syn_backlog=8192
   ```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

# Licensing

The tool is licensed under the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.en.html).

# Legal disclaimer

Usage of this tool to interact with targets without prior mutual consent is illegal. It's the end user's responsibility to obey all applicable local, state and federal laws. Developers assume no liability and are not responsible for any misuse or damage caused by this program. Only use for educational purposes.regulations. 
