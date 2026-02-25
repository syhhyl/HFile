#include "helper.h"
#include "net.h"
#include "server.h"

int server(const char *path, uint16_t port) {
  int exit_code = 0;

  if (path == NULL || strlen(path) == 0) {
    exit_code = 1;
    goto PATH_ERROR;
  }

#ifdef _WIN32
  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  int sock = socket(AF_INET, SOCK_STREAM, 0);  
  if (sock < 0) {
#endif
    sock_perror("socket");
    exit_code = 1;
    return exit_code;
  }

  int opt = 1;

#ifdef _WIN32
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
#else
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
    sock_perror("setsockopt(SO_REUSEADDR)");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

#ifdef _WIN32
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    sock_perror("bind");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  
#ifdef _WIN32
  if (listen(sock, 1) == SOCKET_ERROR) {
#else
  if (listen(sock, 1) == -1) {
#endif
    sock_perror("listen");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  

  printf("listening on %s port %u...\n", path, (unsigned)port);

  int conn_flag = 1;
  while (conn_flag) {
#ifdef _WIN32
    SOCKET conn = accept(sock, NULL, NULL);
    if (conn == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEINTR) continue;
#else
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR) continue;
#endif
      sock_perror("accept");
      continue;
    }
    printf("client connected\n");
  

    
    uint16_t file_len;
    
    if (recv_all(conn, &file_len, sizeof(file_len)) != sizeof(file_len)) {
      sock_perror("recv_all(file_len)");
      exit_code = 1;
      goto CLOSE_CONN;
    }
    
    file_len = ntohs(file_len);
    
    
    if (file_len == 0 || file_len > 255) {
      fprintf(stderr, "invalid file name length: %u\n", (unsigned)file_len);
      goto CLOSE_CONN;
    }

    char *file_name = (char *)malloc((size_t)file_len + 1);
    if (file_name == NULL) {
      perror("malloc(file_name)");
      exit_code = 1;
      goto CLOSE_CONN;
    }
    
    if (recv_all(conn, file_name, (size_t)file_len) != (ssize_t)file_len) {
      sock_perror("recv_all(file_name)");
      free(file_name);
      exit_code = 1;
      goto CLOSE_CONN;
    }

    file_name[file_len] = '\0';

    if (strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL ||
        strstr(file_name, "..") != NULL) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      goto CLOSE_CONN;
    }

    char full_path[4096];
#ifdef _WIN32
    const char *sep = (path[strlen(path)-1] == '\\' || path[strlen(path)-1] == '/') ? "" : "\\";
#else
    const char *sep = (path[strlen(path)-1] == '/') ? "" : "/";
#endif
    snprintf(full_path, sizeof(full_path), "%s%s%s", path, sep, file_name);
    free(file_name);
    

    int out;
#ifdef _WIN32
    out = open(full_path, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY, 0644);
#else
    out = open(full_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
#endif
    if (out == -1) {
      perror("open");
      exit_code = 1;
      goto CLOSE_CONN;
    }

    char *buf = (char *)malloc(CHUNK_SIZE);
    if (buf == NULL) {
      perror("malloc(buf)");
      exit_code = 1;
      goto CLOSE_FILE;
    }

    for (;;) {
      ssize_t n = 0;
#ifdef _WIN32
      int tmp = recv(conn, buf, (int)CHUNK_SIZE, 0);
      if (tmp == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        sock_perror("recv");
        exit_code = 1;
        break;
      }
      n = (ssize_t)tmp;
#else
      n = recv(conn, buf, CHUNK_SIZE, 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        sock_perror("recv");
        exit_code = 1;
        break;
      }
#endif
      if (n == 0) break;
      ssize_t nw = write_all(out, buf, (size_t)n);
      if (nw != (ssize_t)n) {
        perror("write_all");
        exit_code = 1;
        break;
      }
    }

    free(buf);

CLOSE_FILE:
    fd_close(out);

CLOSE_CONN:
    socket_close(conn);
  }
  

CLOSE_SOCK:
  socket_close(sock);
  
PATH_ERROR:
  return exit_code;
}
