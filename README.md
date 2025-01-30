# DoReMiFaSolLaTiDOS - HTTP Load Testing Tool

A lightweight, concurrent HTTP load testing tool written in Go. This tool allows you to perform load testing on web servers by sending multiple concurrent GET or POST requests.

## Features

- Concurrent request handling with configurable number of threads
- Support for both GET and POST requests
- Real-time statistics reporting
- Configurable request timeout
- Graceful shutdown with CTRL+C
- Detailed success/failure statistics
- Connection pooling for better performance
- Proper error handling and reporting

## Installation

Ensure you have Go installed on your system (version 1.13 or later), then:

```bash
# Clone the repository
git clone [your-repository-url]
cd [repository-name]

# Build the binary
go build -o doremifasollatidos
```

## Usage

### Command Line Options

- `-g` : URL for GET request (e.g., `-g 'http://example.com'`)
- `-p` : URL for POST request (e.g., `-p 'http://example.com'`)
- `-d` : Data payload for POST request (required when using POST)
- `-t` : Number of concurrent threads (default: 500)
- `-timeout` : Request timeout duration (default: 10s)

### Examples

#### GET Request
```bash
# Basic GET request with 100 concurrent threads
./doremifasollatidos -g "http://example.com" -t 100

# GET request with custom timeout
./doremifasollatidos -g "http://example.com" -t 100 -timeout 5s
```

#### POST Request
```bash
# POST request with form data
./doremifasollatidos -p "http://example.com" -d "key1=value1&key2=value2" -t 100

# POST request with custom timeout
./doremifasollatidos -p "http://example.com" -d "data=test" -t 50 -timeout 3s
```

## Output

The tool provides real-time statistics including:
- Total number of requests sent
- Successful requests (HTTP 200)
- Failed requests (non-200 responses)
- Throttled requests (HTTP 429)
- Error count (connection/timeout errors)

Example output:
```
Requests: 1500 | Success: 1450 | Failures: 20 | Throttled: 25 | Errors: 5
```

## Features in Detail

### Connection Pooling
The tool implements efficient connection pooling, reusing connections when possible to reduce overhead and improve performance.

### Concurrent Processing
Uses Go's goroutines and channels for efficient concurrent request processing. Each worker runs independently and reports statistics atomically.

### Graceful Shutdown
- Handles CTRL+C (SIGINT) gracefully
- Stops new requests
- Waits for in-flight requests to complete
- Provides final statistics before exit

### Error Handling
- Proper error propagation and reporting
- Detailed error messages for debugging
- Continues operation even when individual requests fail

## Performance Considerations

- The tool uses connection pooling to maximize performance
- Each thread maintains its own connection when possible
- Response bodies are properly drained to allow connection reuse
- Atomic operations ensure thread-safe statistics collection

## Limitations

- Only supports basic GET and POST requests
- POST data must be URL-encoded
- No support for custom headers (except Content-Type for POST)
- No support for HTTPS client certificates
- Maximum timeout is system dependent

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

[Your chosen license]

## Author

[Your name/organization] 