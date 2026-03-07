#include "net.h"
#include "server.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "fs.h"
#include <fcntl.h>
#include "helper.h"
#include "save.h"

#include <errno.h>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif


int server(const char *path, uint16_t port, int perf) {
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

    uint8_t ack = 1;
    uint64_t perf_conn_start_ns = now_ns();
    uint64_t perf_io_ns = 0;
    uint64_t perf_net_ns = 0;
    uint64_t perf_file_bytes = 0;
    uint64_t perf_wire_bytes = 0;
    uint64_t t_ack_start = 0;


    uint16_t file_len;

    uint64_t t_recv_len_start = now_ns();
    if (recv_all(conn, &file_len, sizeof(file_len)) != sizeof(file_len)) {
      perf_net_ns += now_ns() - t_recv_len_start;
      sock_perror("recv_all(file_len)");
      exit_code = 1;
      goto CLOSE_CONN;
    }
    perf_net_ns += now_ns() - t_recv_len_start;
    
    file_len = ntohs(file_len);
    
    
    if (file_len == 0 || file_len > 255) {
      fprintf(stderr, "invalid file name length: %u\n", (unsigned)file_len);
      exit_code = 1;
      goto CLOSE_CONN;
    }

    char *file_name = (char *)malloc((size_t)file_len + 1);
    if (file_name == NULL) {
      perror("malloc(file_name)");
      exit_code = 1;
      goto CLOSE_CONN;
    }

    uint64_t t_recv_name_start = now_ns();
    if (recv_all(conn, file_name, (size_t)file_len) != (ssize_t)file_len) {
      perf_net_ns += now_ns() - t_recv_name_start;
      sock_perror("recv_all(file_name)");
      free(file_name);
      exit_code = 1;
      goto CLOSE_CONN;
    }
    perf_net_ns += now_ns() - t_recv_name_start;

    file_name[file_len] = '\0';

    if (save_validate_file_name(file_name) != 0) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      exit_code = 1;
      goto CLOSE_CONN;
    }

    uint8_t szbuf[8];
    uint64_t t_recv_size_start = now_ns();
    if (recv_all(conn, szbuf, sizeof(szbuf)) != sizeof(szbuf)) {
      perf_net_ns += now_ns() - t_recv_size_start;
      sock_perror("recv_all(file_content_size)");
      exit_code = 1;
      free(file_name);
      goto CLOSE_CONN;
    }
    perf_net_ns += now_ns() - t_recv_size_start;
    uint64_t content_size = decode_u64_be(szbuf);
    perf_file_bytes = content_size;
    perf_wire_bytes = (uint64_t)sizeof(uint16_t) + (uint64_t)file_len +
                      (uint64_t)sizeof(uint64_t) + content_size;

    char full_path[4096];
    int full_n = 0;
    if (save_join_path(full_path, sizeof(full_path), path, file_name) != 0) {
      full_n = -1;
    } else {
      full_n = (int)strlen(full_path);
    }
    free(file_name);

    if (full_n < 0 || (size_t)full_n >= sizeof(full_path)) {
      fprintf(stderr, "output path too long\n");
      exit_code = 1;
      goto CLOSE_CONN;
    }

    char tmp_path[4096];
    tmp_path[0] = '\0';

    int out = -1;
#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = (int)getpid();
#endif
    for (int attempt = 0; attempt < 16; attempt++) {
      if (save_make_temp_path(tmp_path, sizeof(tmp_path), full_path, pid, attempt) != 0) {
        fprintf(stderr, "temp path too long\n");
        exit_code = 1;
        goto CLOSE_CONN;
      }

      uint64_t t_open_start = now_ns();
      out = save_open_temp_file(tmp_path);
      perf_io_ns += now_ns() - t_open_start;
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

    uint64_t remaining = content_size;

    while (remaining > 0) {
      ssize_t n = 0;
      size_t want = CHUNK_SIZE;
      if (remaining < (uint64_t)want) want = (size_t)remaining;

#ifdef _WIN32
      uint64_t t_recv_chunk_start = now_ns();
      int tmp = recv(conn, buf, (int)want, 0);
      if (tmp == SOCKET_ERROR) {
        perf_net_ns += now_ns() - t_recv_chunk_start;
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      perf_net_ns += now_ns() - t_recv_chunk_start;
      n = (ssize_t)tmp;
#else
      uint64_t t_recv_chunk_start = now_ns();
      n = recv(conn, buf, want, 0);
      if (n < 0) {
        perf_net_ns += now_ns() - t_recv_chunk_start;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      perf_net_ns += now_ns() - t_recv_chunk_start;
#endif
      if (n == 0) {
        fprintf(stderr, "unexpected EOF while receiving file\n");
        exit_code = 1;
        ok = 0;
        break;
      }

      uint64_t t_write_start = now_ns();
      ssize_t nw = write_all(out, buf, (size_t)n);
      perf_io_ns += now_ns() - t_write_start;
      if (nw != (ssize_t)n) {
        perror("write_all");
        exit_code = 1;
        ok = 0;
        break;
      }
      remaining -= (uint64_t)n;
    }
    if (remaining == 0) {
      ok = 1;
    }

    free(buf);

CLOSE_FILE:
    hf_close(out);


    if (ok) {
      unsigned long win_err = 0;
      uint64_t t_rename_start = now_ns();
      if (save_finalize_temp_file(tmp_path, full_path, &win_err) != 0) {
        perf_io_ns += now_ns() - t_rename_start;
#ifdef _WIN32
        fprintf(stderr, "MoveFileExA failed (err=%lu)\n", (unsigned long)win_err);
#else
        perror("rename");
#endif
        save_remove_quiet(tmp_path);
        exit_code = 1;
      } else {
        perf_io_ns += now_ns() - t_rename_start;
        ack = 0;
      }
    } else {
      save_remove_quiet(tmp_path);
    }

CLOSE_CONN:
    t_ack_start = now_ns();
    ssize_t ack_sent = send_all(conn, &ack, sizeof(ack));
    perf_net_ns += now_ns() - t_ack_start;
    if (ack_sent != (ssize_t)sizeof(ack)) {
      sock_perror("send_all(ack)");
    }

    if (perf) {
      uint64_t perf_total_ns = now_ns() - perf_conn_start_ns;
      report_transfer_perf(
        "server",
        ack == 0 ? 1 : 0, 
        ns_to_s(perf_total_ns),
        ns_to_s(perf_io_ns), 
        ns_to_s(perf_net_ns), 
        perf_file_bytes, 
        perf_wire_bytes);
    }
    socket_close(conn);
  }
  

CLOSE_SOCK:
  socket_close(sock);
  
PATH_ERROR:
  return exit_code;
}
