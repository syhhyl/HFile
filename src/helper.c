#include "helper.h"



ssize_t send_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  const void *data, size_t len) {
  size_t total = 0;
  const char *p = data;

#ifndef _WIN32
  int flags = 0;
  #ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
  #endif
#endif

  while (total < len) {
#ifdef _WIN32
    int n = send(sock, p+total, (int)(len-total), 0);
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) continue;
      return -1;
    }
#else
    ssize_t n = send(sock, p+total, len-total, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  
  return (ssize_t)total;
}

ssize_t recv_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t total = 0;

  while (total < len) {
#ifdef _WIN32
    int n = recv(sock, (char *)p + total, (int)(len - total), 0);
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) continue;
      return -1;
    }
#else
    ssize_t n = recv(sock, p + total, len - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

ssize_t write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t total = 0;
   
  while (total < len) {
#ifdef _WIN32
    int n = write(fd, p+total, (unsigned)(len-total));
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
#else
    ssize_t n = write(fd, p+total, len-total);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

void sock_perror(const char *msg) {
#ifdef _WIN32
  int err = WSAGetLastError();
  char *sys = NULL;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD n = FormatMessageA(flags, NULL, (DWORD)err,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           (LPSTR)&sys, 0, NULL);
  if (n != 0 && sys != NULL) {
    while (n > 0 && (sys[n - 1] == '\r' || sys[n - 1] == '\n')) {
      sys[n - 1] = '\0';
      n--;
    }
    fprintf(stderr, "%s: %s (WSA=%d)\n", msg, sys, err);
    LocalFree(sys);
  } else {
    fprintf(stderr, "%s: WSA error %d\n", msg, err);
  }
#else
  perror(msg);
#endif
}



void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n",
          argv0, argv0);
}


int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') return 1;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 1;
  if (v == 0 || v > 65535UL) return 1;
  *out = (uint16_t)v;
  return 0;
}

int need_value(int argc, char **argv, int *i, const char **out) {
  if (*i + 1 >= argc) return 1;
  *i = *i + 1;
  *out = argv[*i];
  return 0;
}


parse_result_t parse_args(int argc, char **argv, Opt *opt) {
  if (opt == NULL) return PARSE_ERR;

  opt->mode = init_mode;
  opt->path = NULL;
  opt->ip = "127.0.0.1";
  opt->port = 9000;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL || a[0] != '-' || a[1] == '\0' || a[2] != '\0') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    switch (a[1]) {
      case 'h':
        return PARSE_HELP;

      case 's': {
        if (opt->mode == client_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }

        opt->mode = server_mode;
        opt->path = v;
        break;
      }

      case 'c': {
        if (opt->mode == server_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }

        opt->mode = client_mode;
        opt->path = v;
        break;
      }

      case 'i': {
        if (opt->mode != client_mode) {
          fprintf(stderr, "server mode don't need ip\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }

        opt->ip = v;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        if (parse_port(v, &opt->port) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        break;
      }

      default:
        return PARSE_ERR;
    }
  }

  if (opt->mode == init_mode) {
    return PARSE_ERR;
  }
  if (opt->path == NULL) {
    return PARSE_ERR;
  }

  return PARSE_OK;
}

int get_file_name(const char **file_path, const char **file_name) {
  if (*file_path == NULL) return 1;
  char *tmp;
#ifdef _WIN32
  tmp = strrchr(*file_path, '\\');
#else
  tmp = strrchr(*file_path, '/');
#endif
  if (tmp == NULL) *file_name = *file_path;
  else *file_name = tmp + 1;
  return *file_name ? 0 : 1;
}
