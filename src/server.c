#include "net.h"
#include "server.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "fs.h"
#include <fcntl.h>
#include "helper.h"

#include <errno.h>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

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
  fflush(stdout);

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
    int full_n = snprintf(full_path, sizeof(full_path), "%s%s%s", path, sep, file_name);
    free(file_name);

    if (full_n < 0 || (size_t)full_n >= sizeof(full_path)) {
      fprintf(stderr, "output path too long\n");
      exit_code = 1;
      goto CLOSE_CONN;
    }

    char tmp_path[4096];
    tmp_path[0] = '\0';

    int out = -1;
    for (int attempt = 0; attempt < 16; attempt++) {
#ifdef _WIN32
      int pid = _getpid();
#else
      int pid = (int)getpid();
#endif
      int tmp_n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%d", full_path, pid, attempt);
      if (tmp_n < 0 || (size_t)tmp_n >= sizeof(tmp_path)) {
        fprintf(stderr, "temp path too long\n");
        exit_code = 1;
        goto CLOSE_CONN;
      }

#ifdef _WIN32
      out = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL | O_BINARY, 0644);
#else
      out = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0644);
#endif
      if (out != -1) break;
      if (errno == EEXIST) continue;
      perror("open(temp)");
      exit_code = 1;
      goto CLOSE_CONN;
    }

    if (out == -1) {
      fprintf(stderr, "failed to create temp file\n");
      exit_code = 1;
      goto CLOSE_CONN;
    }
    

    int ok = 0;
    char *buf = (char *)malloc(CHUNK_SIZE);
    if (buf == NULL) {
      perror("malloc(buf)");
      exit_code = 1;
      goto CLOSE_FILE;
    }

    //time test
    double t_recv = 0.0;
    double t_write = 0.0;
    size_t bytes_recv = 0;
    size_t bytes_write = 0;

    for (;;) {
      ssize_t n = 0;
      double t0 = now_sec();
#ifdef _WIN32
      int tmp = recv(conn, buf, (int)CHUNK_SIZE, 0);
      if (tmp == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      n = (ssize_t)tmp;
      t_recv += now_sec() - t0;
#else
      n = recv(conn, buf, CHUNK_SIZE, 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      t_recv += now_sec() - t0;
#endif
      if (n == 0) {
        ok = 1;
        break;
      }
      bytes_recv += (size_t)n;

      double t1 = now_sec(); 
      ssize_t nw = write_all(out, buf, (size_t)n);
      t_write += now_sec() - t1;
      if (nw != (ssize_t)n) {
        perror("write_all");
        exit_code = 1;
        ok = 0;
        break;
      }
      bytes_write += (size_t)nw;
    }

    free(buf);
  
    fprintf(stderr,
      "[timing] recv: %.6fs (%.2f MiB/s), write: %.6fs (%.2f MiB/s)\n",
      t_recv, t_recv > 0 ? (bytes_recv / (1024.0*1024.0)) / t_recv : 0.0,
      t_write, t_write > 0 ? (bytes_write / (1024.0*1024.0)) / t_write : 0.0);   

CLOSE_FILE:
    fd_close(out);

    if (ok) {
#ifdef _WIN32
      if (!MoveFileExA(tmp_path, full_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        fprintf(stderr, "MoveFileExA failed (err=%lu)\n", (unsigned long)err);
        (void)remove(tmp_path);
        exit_code = 1;
      }
#else
      if (rename(tmp_path, full_path) != 0) {
        perror("rename");
        (void)remove(tmp_path);
        exit_code = 1;
      }
#endif
    } else {
      (void)remove(tmp_path);
    }

CLOSE_CONN:
    socket_close(conn);
  }
  

CLOSE_SOCK:
  socket_close(sock);
  
PATH_ERROR:
  return exit_code;
}
