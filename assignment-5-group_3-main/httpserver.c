#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;  // Only used by poolserver
int num_threads;  // Only used by poolserver
int server_port;  // Default value: 8000
char *server_files_directory;
int maximum_file_size = 100;


/* Helper function */

int get_file_size(int fd) {
    int saved_pos = lseek(fd, 0, SEEK_CUR);
    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, saved_pos, SEEK_SET);
    return file_size;
}

int http_send_file(int dst_fd, int src_fd) {
    const int buf_size = 4096;
    char buf[buf_size];
    ssize_t bytes_read;
    int status;
    while( (bytes_read=read(src_fd, buf, buf_size))!=0) {
        status = http_send_data(dst_fd, buf, bytes_read);
        if(status < 0) return status;
    }
    return 0;
}

int http_send_string(int fd, char *data) {
    return http_send_data(fd, data, strlen(data));
}

int http_send_data(int fd, char *data, size_t size) {
    ssize_t bytes_sent;
    while (size > 0) {
        bytes_sent = write(fd, data, size);
        if (bytes_sent < 0)
            return bytes_sent;
        size -= bytes_sent;
        data += bytes_sent;
    }
    return 0;
}

char* combine_string(char *first_string, char *second_string, size_t *size) {
    char *combined_string = (char *) malloc(strlen(first_string) + strlen(second_string) + 1);
    char *p = combined_string;
    for(; (*p=*first_string); p++, first_string++);
    for(; (*p=*second_string); p++, second_string++);
    if(size != NULL) *size = (p-combined_string)*sizeof(char);
    return combined_string;
}



/*
 * Serves the contents the file stored at `path` to the client socket `fd`.
 * It is the caller's reponsibility to ensure that the file stored at `path` exists.
 */
void serve_file(int fd, char *path) {

  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(path));

  int source_fd = open(path,O_RDONLY); //opening the file
  char *file_size = (char *) malloc(maximum_file_size * sizeof(char));
  sprintf(file_size, "%d", get_file_size(source_fd));  
  http_send_header(fd, "Content-Length", file_size); // Change this too

  http_end_headers(fd);

  /* TODO: PART 2 */
  http_send_file(fd, source_fd);
  close(source_fd);
  return;
}

void serve_directory(int fd, char *path) {
  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(".html"));
  http_end_headers(fd);

  /* TODO: PART 3 */
  int index_exist = 0;
  DIR *dirp;
  struct dirent *dp;
  dirp = opendir(path);
  while ((dp = readdir(dirp)) != NULL) { // check if this directory has index.html
    if (strcmp(dp->d_name, "index.html") == 0) {
      int length = strlen(path) + strlen("/index.html") + 1;
      char* full_path = (char* ) malloc(length * sizeof(char));
      http_format_index(full_path, path);

      // send index.html file
      int src_fd = open(full_path,O_RDONLY);
      http_send_file(fd, src_fd);

      close(src_fd);

      index_exist = 1;
      break;
    }
  }

  if (index_exist) {
    closedir(dirp);
    return;
  }


  char* send_buffer = (char *)malloc(1);
  send_buffer[0] = '\0';
  dirp = opendir(path);

  while ((dp = readdir(dirp)) != NULL) {
    char *directory_name = dp->d_name;
    char *file_name_buffer;

    if (strcmp(directory_name, "..") == 0) {
      int length = strlen("<a href=\"//\"></a><br/>") + strlen("..") + strlen("")*2 + 1;
      file_name_buffer = (char* ) malloc(length * sizeof(char));
      http_format_href(file_name_buffer, "..", directory_name);

    } else {
      int length = strlen("<a href=\"//\"></a><br/>") + strlen(path) + strlen(directory_name)*2 + 1;
      file_name_buffer = (char* ) malloc(length * sizeof(char));
      http_format_href(file_name_buffer, path, directory_name);
    }

    send_buffer = combine_string(send_buffer, file_name_buffer, NULL);
    free(file_name_buffer);
  }

  size_t content_len;
  send_buffer = combine_string(send_buffer,  "</center></body></html>", &content_len);

  http_send_string(fd, send_buffer);

  closedir(dirp); 
}

/*
 * Reads an HTTP request from client socket (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 *
 *   Closes the client socket (fd) when finished.
 */
void handle_files_request(int fd) {

  struct http_request *request = http_request_parse(fd);

  if (request == NULL || request->path[0] != '/') {
    http_start_response(fd, 400);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  if (strstr(request->path, "..") != NULL) {
    http_start_response(fd, 403);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  /* Remove beginning `./` */
  char *path = malloc(2 + strlen(request->path) + 1);
  path[0] = '.';
  path[1] = '/';
  memcpy(path + 2, request->path, strlen(request->path) + 1);

  /*
   * TODO: PART 2 is to serve files. If the file given by `path` exists,
   * call serve_file() on it. Else, serve a 404 Not Found error below.
   * The `stat()` syscall will be useful here.
   *
   * TODO: PART 3 is to serve both files and directories. You will need to
   * determine when to call serve_file() or serve_directory() depending
   * on `path`. Make your edits below here in this function.
   */

  struct stat sb;
  int exist = stat(path, &sb);

  if (exist == 0) { // check if path is exist
    if (S_ISDIR(sb.st_mode)) { // check if path is directory
      serve_directory(fd, path);
    } else {
      serve_file(fd, path);
    }

  } else {
    http_start_response(fd, 404);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
  }


  close(fd);
  return;
}



#ifdef POOLSERVER
/* 
 * All worker threads will run this function until the server shutsdown.
 * Each thread should block until a new request has been received.
 * When the server accepts a new connection, a thread should be dispatched
 * to send a response to the client.
 */
void *handle_clients(void *void_request_handler) {
  void (*request_handler)(int) = (void (*)(int)) void_request_handler;
  /* (Valgrind) Detach so thread frees its memory on completion, since we won't
   * be joining on it. */
  pthread_detach(pthread_self());

  /* TODO: PART 7 */

}

/* 
 * Creates `num_threads` amount of threads. Initializes the work queue.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {

  /* TODO: PART 7 */

}
#endif

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  // Creates a socket for IPv4 and TCP.
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  // Setup arguments for bind()
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /* 
   * TODO: PART 1 BEGIN
   *
   * Given the socket created above, call bind() to give it
   * an address and a port. Then, call listen() with the socket.
   * An appropriate size of the backlog is 1024, though you may
   * play around with this value during performance testing.
   */


  if (bind(*socket_number, (struct sockaddr *) &server_address,
            sizeof(server_address)) == -1) {
    perror("Failed to bind on socket");
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
      perror("Failed to listen on socket");
      exit(errno);
  }

  /* PART 1 END */
  printf("Listening on port %d...\n", server_port);

#ifdef POOLSERVER
  /* 
   * The thread pool is initialized *before* the server
   * begins accepting client connections.
   */
  init_thread_pool(num_threads, request_handler);
#endif

  while (1) {
    client_socket_number = accept(*socket_number,
        (struct sockaddr *) &client_address,
        (socklen_t *) &client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

#ifdef BASICSERVER
    /*
     * This is a single-process, single-threaded HTTP server.
     * When a client connection has been accepted, the main
     * process sends a response to the client. During this
     * time, the server does not listen and accept connections.
     * Only after a response has been sent to the client can
     * the server accept a new connection.
     */
    request_handler(client_socket_number);
#elif FORKSERVER
    /* 
     * TODO: PART 5 BEGIN
     *
     * When a client connection has been accepted, a new
     * process is spawned. This child process will send
     * a response to the client. Afterwards, the child
     * process should exit. During this time, the parent
     * process should continue listening and accepting
     * connections.
     */
      





    /* PART 5 END */
#elif THREADSERVER
    /* 
     * TODO: PART 6 BEGIN
     *
     * When a client connection has been accepted, a new
     * thread is created. This thread will send a response
     * to the client. The main thread should continue
     * listening and accepting connections. The main
     * thread will NOT be joining with the new thread.
     */

    /* PART 6 END */
#elif POOLSERVER
    /* 
     * TODO: PART 7 BEGIN
     *
     * When a client connection has been accepted, add the
     * client's socket number to the work queue. A thread
     * in the thread pool will send a response to the client.
     */

    /* PART 7 END */
#endif
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("The signal is caught %d: %s\n", signum, strsignal(signum));
  printf("socket is being closed %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
  "Usage: ./httpserver --files some_directory/ [--port 8000 --num-threads 5]\n"
  "       ./httpserver --proxy inst.eecs.berkeley.edu:80 [--port 8000 --num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
  signal(SIGINT, signal_callback_handler);
  signal(SIGPIPE, SIG_IGN);

  /* Default settings */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char* server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char* num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL) {
    fprintf(stderr, "Please specify \"--files [DIRECTORY]\"\n");
    exit_with_usage();
  }

#ifdef POOLSERVER
  if (num_threads < 1) {
    fprintf(stderr, "Please specify \"--num-threads [N]\"\n");
    exit_with_usage();
  }
#endif

  chdir(server_files_directory);
  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}