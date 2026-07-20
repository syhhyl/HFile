#include "integration_support.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

char hf_path[4096];
char g_project_root[4096];
char test_dir[256];

static void be64_write(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
  p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
  p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
  p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

static void init_hf_path(void) {
  const char *env = getenv("HF_PATH");
  if (env && *env) {
    snprintf(hf_path, sizeof(hf_path), "%s", env);
  } else {
    snprintf(hf_path, sizeof(hf_path), "./build/hf");
  }
}

static pid_t waitpid_retry(pid_t pid, int *status, int options) {
  pid_t result;
  do {
    result = waitpid(pid, status, options);
  } while (result < 0 && errno == EINTR);
  return result;
}

static void terminate_child(pid_t pid) {
  if (pid <= 0) return;
  if (kill(pid, SIGTERM) != 0 && errno != ESRCH) return;

  int status;
  waitpid_retry(pid, &status, 0);
}

static int alloc_port(uint16_t *port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return 1;

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(0);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(sock);
    return 1;
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(sock, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(sock);
    return 1;
  }

  *port = ntohs(addr.sin_port);
  close(sock);
  return 0;
}

int spawn_hf(char *const argv[], char *out_buf, char *err_buf) {
  int out_pipe[2], err_pipe[2];
  if (pipe(out_pipe) < 0) return -1;
  if (pipe(err_pipe) < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    close(out_pipe[0]); close(err_pipe[0]);
    dup2(out_pipe[1], STDOUT_FILENO); close(out_pipe[1]);
    dup2(err_pipe[1], STDERR_FILENO); close(err_pipe[1]);
    execvp(argv[0], argv);
    _exit(127);
  }

  close(out_pipe[1]); close(err_pipe[1]);

  if (out_buf) {
    ssize_t n = read(out_pipe[0], out_buf, OUT_CAP - 1);
    out_buf[(n > 0) ? n : 0] = '\0';
  } else {
    char discard[8192];
    while (read(out_pipe[0], discard, sizeof(discard)) > 0) {}
  }
  close(out_pipe[0]);

  if (err_buf) {
    ssize_t n = read(err_pipe[0], err_buf, OUT_CAP - 1);
    err_buf[(n > 0) ? n : 0] = '\0';
  } else {
    char discard[8192];
    while (read(err_pipe[0], discard, sizeof(discard)) > 0) {}
  }
  close(err_pipe[0]);

  int status;
  if (waitpid_retry(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

enum {
  RECEIVER_READY = 0,
  RECEIVER_FAILED = 1,
  RECEIVER_REAPED = 2,
};

static int wait_receiver_ready(pid_t pid, int output_fd, int timeout_ms) {
  char output[4096];
  size_t used = 0;
  int elapsed = 0;

  while (elapsed < timeout_ms) {
    struct pollfd poll_fd = {
      .fd = output_fd,
      .events = POLLIN,
    };
    int interval = timeout_ms - elapsed < 50 ? timeout_ms - elapsed : 50;
    int poll_result;
    do {
      poll_result = poll(&poll_fd, 1, interval);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) return RECEIVER_FAILED;
    elapsed += interval;

    if (poll_result > 0 && (poll_fd.revents & (POLLIN | POLLHUP))) {
      ssize_t n;
      do {
        n = read(output_fd, output + used, sizeof(output) - used - 1);
      } while (n < 0 && errno == EINTR);
      if (n <= 0) return RECEIVER_FAILED;

      used += (size_t)n;
      output[used] = '\0';
      if (strstr(output, "HFile node ready\n") != NULL) {
        int status;
        pid_t result = waitpid_retry(pid, &status, WNOHANG);
        if (result == 0) return RECEIVER_READY;
        return RECEIVER_REAPED;
      }
      if (used == sizeof(output) - 1) return RECEIVER_FAILED;
    }
    if (poll_result > 0 && (poll_fd.revents & (POLLERR | POLLNVAL))) {
      return RECEIVER_FAILED;
    }

    int status;
    pid_t result = waitpid_retry(pid, &status, WNOHANG);
    if (result == pid || result < 0) return RECEIVER_REAPED;
  }
  return RECEIVER_FAILED;
}

static int start_recv_on_port(const char *dir, uint16_t port,
                              receiver_t *receiver) {
  int output_pipe[2];
  if (pipe(output_pipe) != 0) return 1;

  pid_t pid = fork();
  if (pid < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return 1;
  }
  if (pid == 0) {
    close(output_pipe[0]);
    int flags = fcntl(output_pipe[1], F_GETFL, 0);
    if (flags < 0 || fcntl(output_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0) {
      _exit(1);
    }
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(1);
    close(output_pipe[1]);

    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
      dup2(null, STDERR_FILENO);
      close(null);
    }
    close(STDIN_FILENO);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    execlp(hf_path, hf_path, "recv", dir, "-p", port_str, (char *)NULL);
    _exit(1);
  }

  close(output_pipe[1]);
  int result = wait_receiver_ready(pid, output_pipe[0], 5000);
  if (result != RECEIVER_READY) {
    close(output_pipe[0]);
    if (result != RECEIVER_REAPED) terminate_child(pid);
    return 1;
  }

  receiver->pid = pid;
  receiver->port = port;
  receiver->output_fd = output_pipe[0];
  return 0;
}

int start_recv(const char *dir, receiver_t *receiver) {
  receiver->pid = -1;
  receiver->port = 0;
  receiver->output_fd = -1;

  for (int attempt = 0; attempt < 10; attempt++) {
    uint16_t candidate;
    if (alloc_port(&candidate) != 0) return 1;

    if (start_recv_on_port(dir, candidate, receiver) == 0) return 0;
  }
  return 1;
}

void stop_recv(receiver_t *receiver) {
  terminate_child(receiver->pid);
  if (receiver->output_fd >= 0) close(receiver->output_fd);
  receiver->pid = -1;
  receiver->port = 0;
  receiver->output_fd = -1;
}

int tcp_connect(const char *host, uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return -1;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host, &addr.sin_addr);
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

static ssize_t recv_exact(int sock, void *buf, size_t len) {
  size_t total = 0;
  uint8_t *p = (uint8_t *)buf;
  while (total < len) {
    ssize_t n = recv(sock, p + total, len - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

int send_preamble(int sock, uint16_t magic, uint8_t ver,
                  uint8_t type, uint8_t flags, uint64_t payload,
                  const char *name, uint16_t nlen, uint64_t fsize) {
  uint8_t buf[279];
  memset(buf, 0, sizeof(buf));
  uint8_t *p = buf;
  uint16_t mg = htons(magic);
  memcpy(p, &mg, 2); p += 2;
  *p++ = ver; *p++ = type; *p++ = flags;
  be64_write(p, payload); p += 8;
  uint16_t nb = htons(nlen);
  memcpy(p, &nb, 2); p += 2;
  if (nlen > 0) memcpy(p, name, nlen);
  p += 256;
  be64_write(p, fsize);
  size_t off = 0;
  while (off < sizeof(buf)) {
    ssize_t n = send(sock, buf + off, sizeof(buf) - off, 0);
    if (n < 0) { if (errno == EINTR) continue; return 1; }
    if (n == 0) return 1;
    off += (size_t)n;
  }
  return 0;
}

int recv_response(int sock, uint8_t *phase, uint8_t *status) {
  uint8_t buf[2];
  if (recv_exact(sock, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) return 1;
  *phase = buf[0];
  *status = buf[1];
  return 0;
}

int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

int write_file(const char *path, const void *data, size_t len) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return 1;
  size_t off = 0;
  const uint8_t *p = (const uint8_t *)data;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0) { if (errno == EINTR) continue; close(fd); return 1; }
    off += (size_t)n;
  }
  close(fd);
  return 0;
}

int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

int files_equal(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb");
  FILE *fb = fopen(b, "rb");
  if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
  int c1, c2;
  do {
    c1 = fgetc(fa);
    c2 = fgetc(fb);
    if (c1 != c2) { fclose(fa); fclose(fb); return 0; }
  } while (c1 != EOF);
  fclose(fa); fclose(fb);
  return 1;
}

int rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    if (!dir) return 1;
    struct dirent *entry;
    char sub[1024];
    while ((entry = readdir(dir)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
      rm_rf(sub);
    }
    closedir(dir);
    rmdir(path);
  } else {
    unlink(path);
  }
  return 0;
}

void make_tmp_path(char *buf, size_t cap, const char *name) {
  snprintf(buf, cap, "%s/%s", test_dir, name);
}

static int init_test_dir(void) {
  snprintf(test_dir, sizeof(test_dir), "/tmp/hf_int_test_XXXXXX");
  return mkdtemp(test_dir) ? 0 : 1;
}

int count_tmp_files(const char *dir) {
  DIR *stream = opendir(dir);
  if (!stream) return -1;
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(stream)) != NULL) {
    if (strstr(entry->d_name, ".tmp.")) count++;
  }
  closedir(stream);
  return count;
}

int integration_init(int argc, char **argv) {
  init_hf_path();
  if (argc > 1) snprintf(hf_path, sizeof(hf_path), "%s", argv[1]);

  if (argc > 2) {
    snprintf(g_project_root, sizeof(g_project_root), "%s", argv[2]);
  } else {
    snprintf(g_project_root, sizeof(g_project_root), ".");
  }

  if (init_test_dir()) {
    fprintf(stderr, "failed to create test dir\n");
    return 1;
  }
  return 0;
}

void integration_cleanup(void) {
  rm_rf(test_dir);
}
