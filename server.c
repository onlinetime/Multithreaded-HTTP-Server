// server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>

#include "threadpool.h"

#define MAX_REQUEST_LINE 4000
#define BUFFER_SIZE 4096
#define BACKLOG 5
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;

int port;

// Global thread pool
threadpool *pool;

// Function prototypes
int create_socket(int domain, int type, int protocol);
int bind_socket(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int start_listen(int sockfd, int backlog);
int make_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
void send_500_internal_server_error(int client_fd);
void send_error_response(int client_fd, int status, const char *status_phrase, const char *body);
int handle_client_request_wrapper(void *arg);
void handle_client_request_impl(int client_fd);
void serve_resource(int client_fd, const char *path);
void list_directory(int client_fd, const char *request_path, const char *filesystem_path);
void serve_file(int client_fd, const char *path);
int decode_url(char *src, char *dest);
int parse_request(const char *request_line, char *method, char *path, char *protocol);
char *custom_dirname(char *path);
char *get_mime_type(char *name);
void populate_timebuf(char *timebuf, size_t buf_size);
void graceful_shutdown(int signo);

/* ------------------------------------------------------------------
   1. Wrapper function that matches 'int (*)(void *)'
   ------------------------------------------------------------------ */
int handle_client_request_wrapper(void *arg)
{
    if (arg == NULL) {
        fprintf(stderr, "handle_client_request_wrapper received NULL argument.\n");
        return -1;
    }

    int client_fd = *((int*)arg);
    free(arg);    // Free the allocated memory for client_fd

    handle_client_request_impl(client_fd);
    pthread_mutex_lock(&count_mutex);
    pthread_mutex_unlock(&count_mutex);
    return 0;     // Return value as required by dispatch_fn
}

/* ------------------------------------------------------------------
   2. The real request handler that processes client requests
   ------------------------------------------------------------------ */
void handle_client_request_impl(int client_fd)
{
    char buffer[MAX_REQUEST_LINE];
    memset(buffer, 0, sizeof(buffer));

    // Read the first line of the request (the request line)
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read < 0) {
        perror("recv");
        send_500_internal_server_error(client_fd);
        close(client_fd);
        return;
    }
    if (bytes_read == 0) {
        // The client closed the connection
        close(client_fd);
        return;
    }

    // Parse the request parameters: method, path, protocol
    char method[16], path[256], protocol[16];
    int status = parse_request(buffer, method, path, protocol);

    if (status == 400) {
        send_error_response(client_fd, 400, "Bad Request", "Invalid request format");
        close(client_fd);
        return;
    }
    if (status == 501) {
        send_error_response(client_fd, 501, "Not Implemented", "Method not supported");
        close(client_fd);
        return;
    }
    if (status == 403) {
        send_error_response(client_fd, 403, "Forbidden", "Access denied");
        close(client_fd);
        return;
    }
    if (status == 404) {
        send_error_response(client_fd, 404, "Not Found", "The requested resource was not found on this server");
        close(client_fd);
        return;
    }
    if (status == 302) {
        // Redirect to the corrected path (path + '/')
        char redirect_path[512];
        snprintf(redirect_path, sizeof(redirect_path), "http://localhost:%d%s/", port, path);

        // Create and send the 302 Found response with the Location header
        char timebuf[128];
        populate_timebuf(timebuf, sizeof(timebuf));

        char header[1024];
        int rc = snprintf(header, sizeof(header),
                          "HTTP/1.0 302 Found\r\n"
                          "Location: %s\r\n"
                          "Server: webserver/1.0\r\n"
                          "Date: %s\r\n"
                          "Connection: close\r\n\r\n",
                          redirect_path, timebuf);
        if (rc < 0 || rc >= (int)sizeof(header)) {
            send_500_internal_server_error(client_fd);
            close(client_fd);
            return;
        }

        send(client_fd, header, strlen(header), 0);
        close(client_fd);
        return;
    }
    if (status == 200) {
        // Serve the requested resource
        serve_resource(client_fd, path);
        close(client_fd);
    } else {
        // Unexpected scenario -> 500 Internal Server Error
        send_500_internal_server_error(client_fd);
        close(client_fd);
    }
}

/* ------------------------------------------------------------------
   3. Main function to set up the server and handle connections
   ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    // Register signal handler for graceful shutdown
    signal(SIGINT, graceful_shutdown);

    // Check command-line arguments
    if (argc != 5) {
        fprintf(stderr, "Usage: server <port> <pool-size> <queue-size> <max-number-of-request>\n");
        exit(EXIT_FAILURE);
    }

    port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int queue_size = atoi(argv[3]);
    int max_requests = atoi(argv[4]);

    if (port <= 0 || pool_size <= 0 || queue_size <= 0 || max_requests <= 0) {
        fprintf(stderr, "Usage: server <port> <pool-size> <queue-size> <max-number-of-request>\n");
        exit(EXIT_FAILURE);
    }

    // Initialize thread pool
    pool = create_threadpool(pool_size, queue_size);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        exit(EXIT_FAILURE);
    }

    // Create server socket
    int server_fd = create_socket(AF_INET, SOCK_STREAM, 0);

    // Set socket options to reuse address and port
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        destroy_threadpool(pool);
        exit(EXIT_FAILURE);
    }

    // Define server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind socket
    bind_socket(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    // Listen for incoming connections
    start_listen(server_fd, BACKLOG);

    int request_count = -1;

    // Server loop to accept and dispatch client connections
   while (request_count < max_requests) {
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    int client_fd = make_accept(server_fd, (struct sockaddr *)&client_addr, &client_addrlen);
    if (client_fd < 0) {
        perror("accept");
        continue;
    }

    int *pclient = malloc(sizeof(int));
    if (!pclient) {
        perror("malloc");
        send_500_internal_server_error(client_fd);
        close(client_fd);
        continue;
    }
    *pclient = client_fd;

    dispatch(pool, handle_client_request_wrapper, pclient);
    request_count++;
}

    // After handling max_requests, initiate shutdown
   // printf("Reached maximum number of requests. Shutting down...\n");
    destroy_threadpool(pool);
    close(server_fd);
    return 0;
}

/* ------------------------------------------------------------------
   4. Graceful shutdown handler to clean up resources on signal
   ------------------------------------------------------------------ */
void graceful_shutdown(int signo) {
    printf("\nReceived interrupt signal. Shutting down server...\n");
    destroy_threadpool(pool);
    exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------
   5. Create a socket with error handling
   ------------------------------------------------------------------ */
int create_socket(int domain, int type, int protocol) {
    int sockfd = socket(domain, type, protocol);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    return sockfd;
}

/* ------------------------------------------------------------------
   6. Bind the socket to an address with error handling
   ------------------------------------------------------------------ */
int bind_socket(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (bind(sockfd, addr, addrlen) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    return 0;
}

/* ------------------------------------------------------------------
   7. Start listening on the socket with error handling
   ------------------------------------------------------------------ */
int start_listen(int sockfd, int backlog) {
    if (listen(sockfd, backlog) < 0) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    return 0;
}

/* ------------------------------------------------------------------
   8. Accept a client connection with error handling
   ------------------------------------------------------------------ */
int make_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int client_fd = accept(sockfd, addr, addrlen);
    if (client_fd < 0) {
        perror("accept");
    }
    return client_fd;
}

/* ------------------------------------------------------------------
   9. Send a 500 Internal Server Error response to the client
   ------------------------------------------------------------------ */
void send_500_internal_server_error(int client_fd) {
    send_error_response(client_fd, 500, "Internal Server Error", "Server encountered an error");
}

/* ------------------------------------------------------------------
   10. Send an error response to the client with specified status
   ------------------------------------------------------------------ */
void send_error_response(int client_fd, int status, const char *status_phrase, const char *body) {
    char response[BUFFER_SIZE];
    int rc = snprintf(response, sizeof(response),
                      "HTTP/1.0 %d %s\r\n"
                      "Server: webserver/1.0\r\n"
                      "Connection: close\r\n\r\n%s",
                      status, status_phrase, body);
    if (rc < 0 || rc >= (int)sizeof(response)) {
        // Attempt to send a fallback response
        const char *fallback = "HTTP/1.0 500 Internal Server Error\r\n"
                               "Server: webserver/1.0\r\n"
                               "Connection: close\r\n\r\nServer error.";
        send(client_fd, fallback, strlen(fallback), 0);
        return;
    }
    send(client_fd, response, strlen(response), 0);
}

/* ------------------------------------------------------------------
   11. Parse the HTTP request line and validate it
   ------------------------------------------------------------------ */
int parse_request(const char *request_line, char *method, char *path, char *protocol) {
    char decoded_path[MAX_REQUEST_LINE];
    struct stat path_stat;
    char full_path[2048];

    // Extract METHOD, PATH, PROTOCOL from the request line
    if (sscanf(request_line, "%15s %255s %15s", method, path, protocol) != 3) {
        return 400;  // Invalid request format
    }

    // Check if the method is GET
    if (strcmp(method, "GET") != 0) {
        return 501;  // Method not implemented
    }

    // Decode URL-encoded characters in the path
    if (decode_url(path, decoded_path) < 0) {
        return 400;  // Bad Request (invalid percent-encoded characters)
    }
    strcpy(path, decoded_path);

    // Check if the protocol is HTTP/1.0 or HTTP/1.1
    if (strcmp(protocol, "HTTP/1.0") != 0 && strcmp(protocol, "HTTP/1.1") != 0) {
        return 400;  // Bad Request (unsupported protocol)
    }
    char buf[522];
    memset(buf,0,sizeof(buf));
    getcwd(buf, sizeof(buf));

    // Create the full system path based on the server's root directory (starting from ".")
    snprintf(full_path, sizeof(full_path), ".%s", path);

    // Check if the requested path exists
    if (stat(full_path, &path_stat) < 0) {
        return 404;  // Not Found
    }

    // Check if the path is a directory but does not end with '/'
    if (S_ISDIR(path_stat.st_mode)) {
        size_t len = strlen(path);
        if (len == 0 || path[len - 1] != '/') {
            return 302;  // Found (redirect to corrected directory path)
        }
        return 200;  // Directory is valid and ends with '/'
    }

    // Check if the file is a regular file and has read permissions
    if (!S_ISREG(path_stat.st_mode) || access(full_path, R_OK) != 0) {
        return 403;  // Forbidden (file is not readable or not a regular file)
    }

    // Ensure that all directories in the path have execute permissions
    char *dir_path = strdup(full_path);
    if (!dir_path) {
        return 500;  // Internal Server Error
    }
    char *dir = custom_dirname(dir_path);
    if (dir == NULL || access(dir, X_OK) != 0) {
        free(dir_path);
        return 403;  // Forbidden (directory is not accessible)
    }
    free(dir_path);

    // Everything is fine
    return 200;
}

/* ------------------------------------------------------------------
   12. Decode URL-encoded characters
   ------------------------------------------------------------------ */
int decode_url(char *src, char *dest) {
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)*(src + 1)) && isxdigit((unsigned char)*(src + 2))) {
            // Decode %XX where XX is a hexadecimal number
            char hex[3] = { *(src + 1), *(src + 2), '\0' };
            *dest++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '%' && (!isxdigit((unsigned char)*(src + 1)) || !isxdigit((unsigned char)*(src + 2)))) {
            // Invalid percent-encoding
            return -1;  // Signal an error
        } else if (*src == '+') {
            // Convert '+' to a space
            *dest++ = ' ';
            src++;
        } else {
            // Copy other characters as is
            *dest++ = *src++;
        }
    }
    *dest = '\0';  // Null-terminate the decoded string
    return 0;      // Success
}

/* ------------------------------------------------------------------
   13. Serve the requested resource (file or directory)
   ------------------------------------------------------------------ */
void serve_resource(int client_fd, const char *path) {
    char full_path[2048];
    int rc = snprintf(full_path, sizeof(full_path), ".%s", path);
    if (rc < 0 || rc >= (int)sizeof(full_path)) {
        send_error_response(client_fd, 414, "URI Too Long", "Requested URI is too long.");
        return;
    }

    struct stat st;
    if (stat(full_path, &st) < 0) {
        send_error_response(client_fd, 404, "Not Found", "File or directory not found");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        size_t len = strlen(path);
        if (len > 0 && path[len - 1] != '/') {
            char redirect_path[512];
            rc = snprintf(redirect_path, sizeof(redirect_path), "%s/", path);
            if (rc < 0 || rc >= (int)sizeof(redirect_path)) {
                send_error_response(client_fd, 500, "Internal Server Error", "Redirect path too long.");
                return;
            }

            char timebuf[128];
            populate_timebuf(timebuf, sizeof(timebuf));

            char header[1024];
            rc = snprintf(header, sizeof(header),
                          "HTTP/1.0 302 Found\r\n"
                          "Location: %s\r\n"
                          "Server: webserver/1.0\r\n"
                          "Date: %s\r\n"
                          "Connection: close\r\n\r\n",
                          redirect_path, timebuf);
            if (rc < 0 || rc >= (int)sizeof(header)) {
                send_error_response(client_fd, 500, "Internal Server Error", "Header too long.");
                return;
            }
            send(client_fd, header, strlen(header), 0);
            return;
        }

        // בדיקה אם יש index.html
        char index_file[2048];
        rc = snprintf(index_file, sizeof(index_file), "%s/index.html", full_path);
        if (rc < 0 || rc >= (int)sizeof(index_file)) {
            send_error_response(client_fd, 500, "Internal Server Error", "Index file path too long.");
            return;
        }

        if (access(index_file, R_OK) == 0) {
            serve_file(client_fd, index_file);
        } else {
            // במקום לקרוא לפונקציה עם (full_path, full_path),
            // נקרא עם (path, full_path):
            list_directory(client_fd, path, full_path);
        }
        return;
    }

    if (S_ISREG(st.st_mode) && access(full_path, R_OK) == 0) {
        serve_file(client_fd, full_path);
    } else {
        send_error_response(client_fd, 403, "Forbidden", "Cannot access resource");
    }
}


/* ------------------------------------------------------------------
   14. List directory contents in HTML format
   ------------------------------------------------------------------ */
void list_directory(int client_fd, const char *request_path, const char *filesystem_path)
{
    // כותרות HTTP בסיסיות
    char timebuf[128];
    populate_timebuf(timebuf, sizeof(timebuf)); // מנגנון שמכין Timestamp בפורמט RFC1123
    char header[1024];
    int rc = snprintf(header, sizeof(header),
                      "HTTP/1.0 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Server: webserver/1.0\r\n"
                      "Date: %s\r\n"
                      "Connection: close\r\n\r\n",
                      timebuf);
    if (rc < 0 || rc >= (int)sizeof(header)) {
        send_error_response(client_fd, 500, "Internal Server Error", "Header too long.");
        return;
    }
    send(client_fd, header, strlen(header), 0);

    // פתיחת התיקייה על דיסק עם filesystem_path
    DIR *dir = opendir(filesystem_path);
    if (!dir) {
        send_error_response(client_fd, 403, "Forbidden", "Cannot access directory");
        return;
    }

    // פתיחת HTML
    char start_html[512];
    rc = snprintf(start_html, sizeof(start_html),
                  "<html><head><title>Index of %s</title></head><body>"
                  "<style>"
                  "th, td { padding: 4px; text-align: left; }"
                  "</style>"
                  "<h1>Index of %s</h1><hr><table><tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>",
                  request_path, request_path);
    if (rc < 0 || rc >= (int)sizeof(start_html)) {
        closedir(dir);
        send_error_response(client_fd, 500, "Internal Server Error", "HTML too long.");
        return;
    }
    send(client_fd, start_html, strlen(start_html), 0);

    // מעבר על כל קובץ/תיקייה בתיקייה
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char real_path[1024];
        snprintf(real_path, sizeof(real_path), "%s/%s", filesystem_path, entry->d_name);

        struct stat sb;
        if (stat(real_path, &sb) == -1) {
            continue; // מתקדם הלאה אם לא ניתן לבצע stat
        }

        // זמן עדכון אחרון כתצוגה
        char mod_time[64];
        strftime(mod_time, sizeof(mod_time), "%Y-%m-%d %H:%M:%S", localtime(&sb.st_mtime));

        // גודל או "-"
        char size[32];
        if (S_ISDIR(sb.st_mode)) {
            snprintf(size, sizeof(size), "-");
        } else {
            snprintf(size, sizeof(size), "%lld bytes", (long long)sb.st_size);
        }

        // הרכבת URL: כאן נשתמש אך ורק ב־request_path, בודקים אם כבר יש '/' בסוף
        char href[2048];
        if (request_path[strlen(request_path) - 1] == '/') {
            snprintf(href, sizeof(href), "%s%s", request_path, entry->d_name);
        } else {
            snprintf(href, sizeof(href), "%s/%s", request_path, entry->d_name);
        }

        char line[1024];
        rc = snprintf(line, sizeof(line),
                      "<tr><td><a href=\"%s\">%s</a></td>"
                      "<td>%s</td><td>%s</td></tr>\n",
                      href, entry->d_name, mod_time, size);
        if (rc < 0 || rc >= (int)sizeof(line)) {
            continue; // מדלגים על שורות ארוכות מדי
        }
        send(client_fd, line, strlen(line), 0);
    }

    closedir(dir);

    // סיום HTML
    const char *end_html = "</table><hr></body></html>";
    send(client_fd, end_html, strlen(end_html), 0);
}


/* ------------------------------------------------------------------
   15. Serve a file to the client, including HTTP headers
   ------------------------------------------------------------------ */
void serve_file(int client_fd, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        send_500_internal_server_error(client_fd);
        return;
    }

    // Get MIME type
    char *mime_type = get_mime_type((char *)path);

    // Prepare HTTP headers
    char timebuf[128];
    populate_timebuf(timebuf, sizeof(timebuf));

    struct stat st;
    if (stat(path, &st) < 0) {
        perror("stat");
        close(file_fd);
        send_error_response(client_fd, 500, "Internal Server Error", "Failed to retrieve file stats.");
        return;
    }

    char header[1024];
    int rc = snprintf(header, sizeof(header),
                      "HTTP/1.0 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Last-Modified: %s\r\n"
                      "Server: webserver/1.0\r\n"
                      "Date: %s\r\n"
                      "Connection: close\r\n\r\n",
                      mime_type ? mime_type : "text/plain",
                      (long)st.st_size,
                      "",  // Placeholder for Last-Modified if needed
                      timebuf);
    if (rc < 0 || rc >= (int)sizeof(header)) {
        perror("snprintf");
        close(file_fd);
        send_error_response(client_fd, 500, "Internal Server Error", "Header too long.");
        return;
    }
    send(client_fd, header, strlen(header), 0);

    // Send file content
    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(file_fd, buffer, sizeof(buffer))) > 0) {
        if (send(client_fd, buffer, bytes, 0) < 0) {
            perror("send");
            break;
        }
    }
    close(file_fd);
}

/* ------------------------------------------------------------------
   16. Extract directory name from a path
   ------------------------------------------------------------------ */
char *custom_dirname(char *path) {
    char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        return NULL; // No directory part
    }
    *last_slash = '\0'; // Split the string at the last '/'
    return path;
}

/* ------------------------------------------------------------------
   17. Get MIME type based on file extension
   ------------------------------------------------------------------ */
char *get_mime_type(char *name) {
    char *ext = strrchr(name, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".au") == 0) return "audio/basic";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    return "text/plain";
}

/* ------------------------------------------------------------------
   18. Populate time buffer with current time in RFC1123 format
   ------------------------------------------------------------------ */
void populate_timebuf(char *timebuf, size_t buf_size) {
    time_t now = time(NULL);
    strftime(timebuf, buf_size, RFC1123FMT, gmtime(&now));
}
