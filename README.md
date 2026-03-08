# Multithreaded HTTP Server & Threadpool

## 📝 Description
This program implements a simple threadpool for handling tasks and a basic HTTP server that uses this threadpool to process incoming client requests concurrently. The threadpool module manages a set of worker threads that pick up tasks from a queue. The server module sets up a socket, accepts connections, and serves requested resources (files or directories).

## ✨ Key Features
- The threadpool and server modules work together so multiple client requests can be processed concurrently.
- The threadpool uses condition variables and mutexes to ensure thread-safe interactions with the task queue.
- The server implements a minimal HTTP/1.0-compliant response mechanism and serves static files or a directory listing. 
- Directories are handled with automatic redirection if the path does not end in a slash.
- Both modules feature robust error handling to gracefully manage invalid requests and resource access.

---

## ⚙️ Architecture & Core Modules

### 🧵 Threadpool Module
- **`create_threadpool`**: Allocates and initializes the threadpool structure. Creates a set of worker threads by calling `pthread_create` for each thread. Prepares the task queue, condition variables, and mutex locks for task synchronization.
- **`dispatch`**: Accepts a function pointer and argument, packaging them into a task. Adds the newly created task to the end of the threadpool’s queue in a thread-safe manner. Notifies worker threads if the queue transitions from empty to not empty.
- **`do_work`**: Worker thread function that runs in a loop. Waits for tasks to be added to the queue, removes them, and executes the supplied function with the given argument. Continues until a shutdown signal is received.
- **`destroy_threadpool`**: Prevents new tasks from being accepted and waits for the worker threads to finish all tasks in the queue. Signals all threads so they can exit. Cleans up memory for the queue, worker threads, mutexes, and condition variables.

### 🌐 HTTP Server Module
- **`main`**: Sets up the server parameters (port, threadpool size, queue size, and maximum requests) and creates the threadpool. Binds to the specified port, configures the socket to listen for incoming connections, and enters a loop that accepts connections. For each accepted connection, dispatches a request handler function to the threadpool. After reaching the maximum number of requests, shuts down the threadpool and closes the server.
- **`handle_client_request_impl`**: Receives and parses the initial request line from the client. Validates the HTTP method (GET), requested path, and protocol. Depending on the parse result, sends an appropriate response (200, 302, 400, 403, 404, 501, or 500). If valid, delegates to `serve_resource` to handle directories or files.
- **`handle_client_request_wrapper`**: A wrapper function that extracts the file descriptor for the client connection. Calls the main request-handling function, handles any final synchronization steps, and returns a value to satisfy the function pointer signature.

### 📂 Request Routing & Resource Serving
- **`parse_request`**: Extracts the method, path, and protocol from the request line. Checks for valid HTTP methods (only GET is supported) and protocols (HTTP/1.0 or HTTP/1.1). Decodes any URL-encoded characters and ensures the file or directory is accessible. Returns status codes such as 200, 302, 400, 403, 404, 501, or 500 based on various conditions.
- **`serve_resource`**: Checks if the requested path refers to a directory or a regular file. For directories, if there is an `index.html`, it serves that file; otherwise, it generates and sends a directory listing. For files, calls `serve_file` if the file can be accessed, or sends an error response.
- **`list_directory`**: Generates an HTML response listing the contents of a directory. Displays each item’s name, last modification time, and size, with links to navigate into subdirectories or files.
- **`serve_file`**: Opens the specified file and sends it to the client in chunks. Prepares HTTP headers with appropriate MIME types and proper content-length. Handles errors gracefully if the file is not found or cannot be opened.

### 🛠️ Utilities
- **`decode_url`**: Decodes percent-encoded characters (e.g., `%20`) and replaces them with their ASCII equivalents. Converts `+` characters into spaces. Returns -1 if it encounters invalid percent-encoding.
- **`custom_dirname`**: Extracts the directory part of a path by locating and trimming at the final slash. Used to ensure directory permissions can be checked.
- **`get_mime_type`**: Inspects a filename’s extension and returns a MIME type string (e.g., `text/html` for `.html` files). Defaults to `text/plain` if the extension is not recognized.
- **`populate_timebuf`**: Retrieves the current time and formats it as an RFC1123 date string (GMT). Used for sending accurate `Date` or other time-related headers.

---

## 🌍 Global Variables
- **`port`**: Stores the server listening port.
- **`pool`**: A global pointer to the threadpool used for dispatching tasks.
- **`count_mutex`**: A mutex used for synchronization in the server code.
