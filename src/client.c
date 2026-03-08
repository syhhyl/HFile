#include "helper.h"
#include "net.h"
#include "protocol.h"
#include "client.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include "fs.h"
#include <unistd.h>
#include <sys/stat.h>


int client(const char *path, const char *ip, uint16_t port, int perf) {
  int exit_code = 0;
  uint64_t perf_start_ns = now_ns();
  uint64_t perf_io_ns = 0;
  uint64_t perf_net_ns = 0;
  uint64_t perf_file_bytes = 0;
  uint64_t perf_wire_bytes = 0;


  

  const char *file_name;
  if (get_file_name(&path, &file_name) != 0) {
    fprintf(stderr, "invalid file path\n");
    exit_code = 1;
    goto EXIT;
  }

  uint16_t file_name_len = 0;
  if (protocol_get_file_name_len(file_name, &file_name_len) != 0) {
    fprintf(stderr, "file name len > 255\n");
    exit_code = 1;
    goto EXIT;
  }


  int in = -1;
  char *buf = NULL;
#ifdef _WIN32
  uint64_t t_open_start = now_ns();
  in = hf_open(path, O_RDONLY | O_BINARY, 0);
#else
  uint64_t t_open_start = now_ns();
  in = hf_open(path, O_RDONLY, 0);
#endif
  perf_io_ns += now_ns() - t_open_start;
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLOSE_FILE;
  }

  if (protocol_header_size(file_name_len) > CHUNK_SIZE) {
    fprintf(stderr, "file name too long for buffer\n");
    exit_code = 1;
    goto CLOSE_FILE;
  }

  buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    exit_code = 1;
    goto CLOSE_FILE;
  } 

  uint64_t content_size = 0;
#ifdef _WIN32
  struct _stat64 st;
  uint64_t t_stat_start = now_ns();
  if (_fstat64(in, &st) != 0) {
    perror("_fstat64");
#else
  struct stat st;
  uint64_t t_stat_start = now_ns();
  if (fstat(in, &st) != 0) {
    perror("fstat");
#endif
    perf_io_ns += now_ns() - t_stat_start;
    exit_code = 1;
    goto CLOSE_FILE;
  }
  perf_io_ns += now_ns() - t_stat_start;
  if (st.st_size < 0) {
    fprintf(stderr, "invalid file size\n");
    exit_code = 1;
    goto CLOSE_FILE;
  }

#ifdef _WIN32
  SOCKET sock = INVALID_SOCKET;
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  int sock = -1;
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
#endif
    sock_perror("socket");
    exit_code = 1;
    if (perf) {
      uint64_t perf_total_ns = now_ns() - perf_start_ns;
      report_transfer_perf(
        "client",
        0,
        ns_to_s(perf_total_ns),
        0,
        0,
        0,
        0
        );
    }
    goto CLOSE_FILE;
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    perror("inet_pton");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

#ifdef _WIN32
  uint64_t t_connect_start = now_ns();
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    perf_net_ns += now_ns() - t_connect_start;
#else
  uint64_t t_connect_start = now_ns();
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perf_net_ns += now_ns() - t_connect_start;
#endif
    sock_perror("connect");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  perf_net_ns += now_ns() - t_connect_start;

  content_size = (uint64_t)st.st_size;
  perf_file_bytes = content_size;
  perf_wire_bytes = (uint64_t)protocol_header_size(file_name_len) +
                    content_size;

  uint64_t t_header_start = now_ns();
  protocol_result_t header_res =
    protocol_send_header(sock, file_name, content_size);
  perf_net_ns += now_ns() - t_header_start;
  if (header_res != PROTOCOL_OK) {
    if (header_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "file name len > 255\n");
    } else {
      sock_perror("protocol_send_header");
    }
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  size_t pos = 0;
  uint64_t remaining = content_size;
  
  //send loop
  for (;;) {
    while (pos < CHUNK_SIZE && remaining > 0) {
      size_t want = CHUNK_SIZE - pos;
      if ((uint64_t)want > remaining) {
        want = (size_t)remaining;
      }

      uint64_t t_read_start = now_ns();
      ssize_t tmp = hf_read(in, buf + pos, want);
      perf_io_ns += now_ns() - t_read_start;
      if (tmp < 0) {
        perror("read");
        exit_code = 1;
        goto CLOSE_SOCK;
      }
      if (tmp == 0) {
        fprintf(stderr, "source file truncated while reading\n");
        exit_code = 1;
        goto CLOSE_SOCK;
      }
      pos += (size_t)tmp;
      remaining -= (uint64_t)tmp;
    }

    if (pos > 0) {
      uint64_t t_send_start = now_ns();
      ssize_t sent = send_all(sock, buf, pos);
      perf_net_ns += now_ns() - t_send_start;
      if (sent != (ssize_t)pos) {
        sock_perror("send");
        exit_code = 1;
        goto CLOSE_SOCK;
      }
      pos = 0;
    }
    
    if (remaining == 0) break;
  }

#ifdef _WIN32
  if (shutdown(sock, SD_SEND) == SOCKET_ERROR) {
    sock_perror("shutdown(SD_SEND)");
  }
#else
  if (shutdown(sock, SHUT_WR) < 0) {
    sock_perror("shutdown(SHUT_WR)");
  }
#endif

  uint8_t ack = 1;
  uint64_t t_ack_start = now_ns();
  if (recv_all(sock, &ack, sizeof(ack)) != (ssize_t)sizeof(ack)) {
    perf_net_ns += now_ns() - t_ack_start;
    sock_perror("recv_all(ack)");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  perf_net_ns += now_ns() - t_ack_start;
  if (ack != 0) {
    fprintf(stderr, "server returned error ack: %u\n", (unsigned)ack);
    exit_code = 1;
    goto CLOSE_SOCK;
  }
   

CLOSE_SOCK:
#ifdef _WIN32
  if (sock != INVALID_SOCKET)
    socket_close(sock);
#else
  if (sock != -1)
    socket_close(sock);
#endif

CLOSE_FILE:
  if (buf != NULL) free(buf);
  if (in != -1) hf_close(in);

EXIT:
  if (perf) {
    uint64_t perf_total_ns = now_ns() - perf_start_ns;
    double total_s = ns_to_s(perf_total_ns);
    report_transfer_perf(
      "client",
      exit_code == 0 ? 1 : 0,
      total_s,
      ns_to_s(perf_io_ns),
      ns_to_s(perf_net_ns),
      perf_file_bytes,
      perf_wire_bytes);
  }

  return exit_code;
}
