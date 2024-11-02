/* 
 * echoserver.c - A simple connection-based echo server 
 * added support for multiple threads
 * usage: echoserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>

#define BUFSIZE 1024

// Global variables
char *document_root; /* document root */
int portno;          /* port to listen on */
int number_of_clients = 0; /* Counter for how many clients we have*/

typedef struct {
  char method[8];
  char path[256];
  char http_version[16];
} HttpRequest;
  
void close_client(int connfd);

void create_headers(char *response, size_t length);
void add_date_header(char *response);

void *run_thread(void *vargp);

int parse_request(int connfd, char *request, char *response, HttpRequest *http_request);

void *create_path(char *filepath, HttpRequest *http_request);

void *check_file(int connfd, char *filepath, char *response, struct stat *file_stat, HttpRequest *http_request);

void transmit(const char *filepath, char *response, const char *http_version, int connfd);

char *get_file_type(const char *filename);


int main(int argc, char **argv) {
  int listenfd;        /* listening socket */
  int *connfd;         /* connection socket */
  socklen_t clientlen; /* byte size of client's address */
  pthread_t tid;       /* thread id */
  int optval;
  
  
  struct sockaddr_in myaddr;  /* my ip address info */
  struct sockaddr clientaddr; /* client's info */

  /* check command line args */
  if (argc != 5) {
    fprintf(stderr, "usage: %s -document_root <document root> -port <port>\n", argv[0]);
    exit(1);
  }
  document_root = argv[2];
  portno = atoi(argv[4]);

  /* first, load up address structs */
  myaddr.sin_port = htons(portno);
  myaddr.sin_family = AF_INET;
  myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    
  /* make a (server) socket */
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) {
    printf("ERROR opening socket\n");
    exit(1);
  }

  /* setsockopt: Handy debugging trick that lets 
   * us rerun the server on same port immediately after we kill it; 
   * otherwise we have to wait about 20 secs. 
   * Eliminates "ERROR on binding: Address already in use" error. 
   */
  optval = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

  /* bind: associate the listening socket (listenfd) with a specific address/port */
  if (bind(listenfd, (struct sockaddr*) &myaddr, sizeof(myaddr)) <0) {
    printf("ERROR on binding\n");
    exit(1);
  }

  /* listen: make it a listening socket ready to accept connection requests */
  if (listen(listenfd, 100) < 0) {
    printf("ERROR on listen\n");
    exit(1);
  }

  /* main loop: wait for a connection request, 
     accept new connection from incoming client
     create a new thread for the client
     close connection. */
  while (1) {

    /* accept: wait for a connection request */
    clientlen = sizeof(clientaddr);

    connfd = malloc(sizeof(int));  //allocation fd on heap, this is thread-safe!
    /* accept new connection from incoming client */
    *connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);

    if (connfd < 0) {
      printf("ERROR on accept\n");
      exit(1);
    }

    printf("Client connected!\n");
    number_of_clients +=1;
    printf("Number of clients: %d \n", number_of_clients);
    /* try to create thread and run function "run_thread" */
    if (pthread_create(&tid, NULL, run_thread, connfd) !=0) {
      printf("error creating threads\n");
      exit(1);
    }

    
  }
  return 0;
}

void *run_thread(void *vargp) {
  char buf[BUFSIZE];             /* message buffer */
  char request[BUFSIZE];          /* Buffer to accumulate the full request */
  int num_read;                  /* num bytes read */
  int total_read = 0;            /* keep track of total bytes read */
  int connfd = *((int *)vargp);  /* nasty casting, but this is our client fd */
  HttpRequest http_request;      /* struct for parsing request */
  char filepath[BUFSIZE];        /* complete filepath of target file */
  char response[BUFSIZE];        /* initial response line */
  // int num_sent;                  /* num bytes sent */
  int parsed;
  struct timeval tv;
  struct stat file_stat;

  /* detach this thread from parent thread */
  if (pthread_detach(pthread_self()) != 0){
    printf("error detaching\n");
    shutdown(connfd, 0);
    close(connfd);
    return 0;
  }    

  /* free heap space for vargp since we have connfd */
  free(vargp); 

  /* read: read HTTP request from the client until a blank line is found */
  bzero(request, BUFSIZE);
  while (1) {
    /* Use setsockopt and call recv*/
    if (number_of_clients == 0){
      tv.tv_sec = 10;
    } else {
      tv.tv_sec = 10*(1/number_of_clients);
    }
    tv.tv_usec = 0;
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    num_read = recv(connfd, buf, BUFSIZE - 1, 0);

    /* check timeout, close connection if timeout */
    if (num_read < 0) {
        printf("Time Out reached\n");
        close_client(connfd);
        return 0;
    }
    /* ensure we don't overflow the request buffer */
    else if (total_read + num_read >= BUFSIZE) {
        printf("ERROR: Request too large\n");
        close_client(connfd);
        return 0;
    }

    /* copy the received data into the request buffer at the correct position */
    memcpy(request + total_read, buf, num_read);
    total_read += num_read;

    /* null-terminate the request string */
    request[total_read] = '\0';

    /* check for a blank line */
    if (strstr(request, "\r\n\r\n") != NULL || strstr(request, "\n\n") != NULL) {

        /* parse HTTP request */
        parsed = parse_request(connfd, request, response, &http_request);

        if (parsed) {
            /* create full filepath for target file */
            create_path(filepath, &http_request);

            /* determine if target file exists and if world-readable permissions are set properly (return error otherwise) */
            check_file(connfd, filepath, response, &file_stat, &http_request);

            /* transmit contents of file to connect (by performing reads on the file and writes on the socket) */
            /* If the file is world-readable, transmit its contents */
            /* This function reads the file contents and sends them to the client over the socket (connfd).*/
            if (file_stat.st_mode & S_IROTH) {
                transmit(filepath, response, http_request.http_version, connfd);
            }

            /* If it is HTTP/1.0 close the connection */
            if (strcmp(http_request.http_version,"HTTP/1.0") == 0){
                close_client(connfd);
                return 0;
            }

            /* If it is HTTP/1.1, loop back to the beginning */
            total_read = 0;
            bzero(request, BUFSIZE);
        } else {
            close_client(connfd);
            return 0;
        }
    }
  }
  return 0;
}

void close_client(int connfd) {
    number_of_clients -=1;
    printf("Number of clients: %d \n", number_of_clients);
    shutdown(connfd, 0);
    close(connfd);
}

void create_headers(char *response, size_t length){
  time_t now;                    /* current time */
  struct tm *tm_info;            /* time structure */
  char date_buffer[30];          /* buffer for formatted date */
  time(&now);
  tm_info = gmtime(&now); // Convert to GMT
  strftime(date_buffer, sizeof(date_buffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);
  snprintf(response, BUFSIZE,
             "Date: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n\r\n", 
             date_buffer, "text/html", length);
}

// Function to create the Date header
void add_date_header(char *response) {
    time_t now;                    /* Current time */
    struct tm *tm_info;            /* Time structure */
    char date_buffer[100];         /* Buffer for formatted date */

    time(&now);
    tm_info = gmtime(&now);  // Convert to GMT
    strftime(date_buffer, sizeof(date_buffer), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", tm_info);
    strncat(response, date_buffer, BUFSIZE - strlen(response) - 1);  // Add Date header to response buffer
}

int parse_request(int connfd, char *request, char *response, HttpRequest *http_request) {
  int num_sent;                  /* num bytes sent */
  int num_parsed = sscanf(request, "%s %s %s", http_request->method, http_request->path, http_request->http_version);
  /* HTML template for error messages */
  char *html_template_400 = "<html><head><title>400 Bad Request</title></head>"
                                    "<body><h1>400 Bad Request</h1></body></html> \n";

  /* check if number of arguments on initial request is correct */
  if (num_parsed == 3) {
    /* check for well-formed request (return error otherwise) */
    if (strcmp(http_request->method, "GET") == 0 &&        // Compare method
        http_request->path[0] == '/' &&                    // Check if path starts with '/'
        ((strcmp(http_request->http_version, "HTTP/1.0") == 0) || // Check for HTTP version
        ((strcmp(http_request->http_version, "HTTP/1.1") == 0) && (strstr(request, "\nHost: ") != NULL)))) {
        return 1;
    }
  }

  /* parsing error, return 404 malformed request */
  if ((strcmp(http_request->http_version, "HTTP/1.0") == 0) || (strcmp(http_request->http_version, "HTTP/1.1") == 0)) {
    snprintf(response, BUFSIZE, "%s 400 Bad Request\n\n", http_request->http_version);
  } else {
    snprintf(response, BUFSIZE, "400 Bad Request\n\n");
  }
  add_date_header(response);
  snprintf(response + strlen(response), BUFSIZE - strlen(response), 
            "Content-Length: %ld\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n", strlen(html_template_400));
  strncat(response, html_template_400, BUFSIZE - strlen(response) - 1);  // Append HTML body to buffer
  num_sent = send(connfd, response, strlen(response), 0);
  if (num_sent < 0) {
      printf("ERROR writing to socket\n");
  }

  return 0;
}

void *create_path(char *filepath, HttpRequest *http_request) {
    /* create full filepath for target file */
    strcpy(filepath, document_root);
    /* check for '/' case, add 'index.html' */
    if (strcmp(http_request->path, "/") == 0) {
        strcat(filepath, "/index.html");
    } else {
        strcat(filepath, http_request->path);
    }
    return 0;
}

void *check_file(int connfd, char *filepath, char *response, struct stat *file_stat, HttpRequest *http_request) {
    char buf[BUFSIZE];             /* Buffer for response */
    int num_sent;                  /* Number of bytes sent */
    
    /* HTML templates for error messages */
    char *html_template_404 = "<html><head><title>404 Not Found</title></head>"
                              "<body><h1>404 File Not Found</h1></body></html> \n";
    char *html_template_403 = "<html><head><title>403 Forbidden</title></head>"
                              "<body><h1>403 Forbidden</h1></body></html> \n";
    
    /* Clear response and buffer */
    bzero(response, BUFSIZE);
    bzero(buf, BUFSIZE);

    /* Use stat() to retrieve file information */
    if (stat(filepath, file_stat) < 0) {
        /* File does not exist: 404 Not Found */
        snprintf(buf, BUFSIZE, "%s 404 Not Found\r\n", http_request->http_version);  // Add response line to buffer
        add_date_header(buf);  // Add Date header to buffer
        snprintf(buf + strlen(buf), BUFSIZE - strlen(buf), 
                 "Content-Length: %ld\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n", strlen(html_template_404));
        strncat(buf, html_template_404, BUFSIZE - strlen(buf) - 1);  // Append HTML body to buffer

    } else {
        /* Check if the file is world-readable (S_IROTH permission) */
        if (!(file_stat->st_mode & S_IROTH)) {
            /* File is not world-readable: 403 Forbidden */
            snprintf(buf, BUFSIZE, "%s 403 Forbidden\r\n", http_request->http_version);  // Add response line to buffer
            add_date_header(buf);  // Add Date header to buffer
            snprintf(buf + strlen(buf), BUFSIZE - strlen(buf), 
                     "Content-Length: %ld\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n", strlen(html_template_403));
            strncat(buf, html_template_403, BUFSIZE - strlen(buf) - 1);  // Append HTML body to buffer
        } else {
            /* File exists and is world-readable: 200 OK */
            snprintf(buf, BUFSIZE, "%s 200 OK\r\n", http_request->http_version);
        }
    }

    /* Send the entire response (headers and body) at once */
    num_sent = send(connfd, buf, strlen(buf), 0);
    if (num_sent < 0) {
        printf("ERROR writing to socket\n");
        return 0;
    }

    return 0;  /* Return NULL to indicate the function completed successfully */
}

void transmit(const char *filepath, char *response, const char *http_version, int connfd) {
    char buf[BUFSIZE];             /* buffer for reading file */
    int num_read;                  /* num bytes read */
    int num_sent;                  /* num bytes sent */
    struct stat file_info;         /* for getting file info */
    time_t now;                    /* current time */
    struct tm *tm_info;            /* time structure */
    char date_buffer[30];          /* buffer for formatted date */
    char *content_type;

    /* HTML template for error messages */
    char *html_template_500 = "<html><head><title>500 Internal Server Error</title></head>"
                                    "<body><h1>500 Internal Server Error</h1></body></html> \r\n\r\n"; 
    /* Open file and transmit contents */
    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        bzero(response, BUFSIZE);
        snprintf(response, BUFSIZE, "%s 500 Internal Server Error\r\n\r\n", http_version);
        num_sent = send(connfd, response, strlen(response), 0);
        if (num_sent < 0) {
            printf("ERROR writing to socket\n");
        }

        bzero(response, BUFSIZE);
        create_headers(response, strlen(html_template_500));
        snprintf(response + strlen(response), BUFSIZE - strlen(response), html_template_500, http_version);
        num_sent = send(connfd, response, strlen(response), 0);
        if (num_sent < 0) {
            perror("ERROR writing to socket");
        }
        close(connfd);
        return;
    }

    /* Get file info for Content-Length */
    if (stat(filepath, &file_info) == -1) {
        perror("ERROR getting file info");
        fclose(file);
        close(connfd);
        return;
    }

    /* Get current time for Date header */
    time(&now);
    tm_info = gmtime(&now); // Convert to GMT
    strftime(date_buffer, sizeof(date_buffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);
    content_type = get_file_type(filepath);

    /* Prepare response headers */
    bzero(response, BUFSIZE);
    snprintf(response, BUFSIZE,
             "Date: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n\r\n", 
             date_buffer, content_type, file_info.st_size);

    /* Send response headers */
    num_sent = send(connfd, response, strlen(response), 0);
    if (num_sent < 0) {
        perror("ERROR writing headers to socket");
        fclose(file);
        close(connfd);
        return;
    }

    /* Read file and send data */
    while ((num_read = fread(buf, 1, BUFSIZE, file)) > 0) {
        num_sent = send(connfd, buf, num_read, 0);
        if (num_sent < 0) {
            perror("ERROR writing to socket");
            fclose(file);
            close(connfd);
            return;
        }
    }

    fclose(file); // Ensure the socket is closed after transmission
}

char *get_file_type(const char *filename) {
    /*Finds the last occurrence of . in the file name, which should separate the file name from the extension.*/
    char *extension = strrchr(filename, '.');

    // If there's no '.' or it's the first character, return "unknown"
    if (!extension || extension == filename) {
        return "unknown";
    }

    extension++; // Skip the '.'

    if (strcmp(extension, "txt") == 0) return "text/plain";
    if (strcmp(extension, "html") == 0) return "text/html";
    if ((strcmp(extension, "jpg") == 0) || (strcmp(extension, "jpeg") == 0)) return "image/jpeg";
    if (strcmp(extension, "gif") == 0) return "image/gif";

    return 0;
}