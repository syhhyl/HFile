#include "test.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define OUT_CAP 65536
static char hf_path[4096];
static char g_project_root[4096];
static char test_dir[256];
static uint16_t next_port = 19900;

static void be64_write(uint8_t *p, uint64_t v) {
  p[0]=(uint8_t)(v>>56); p[1]=(uint8_t)(v>>48);
  p[2]=(uint8_t)(v>>40); p[3]=(uint8_t)(v>>32);
  p[4]=(uint8_t)(v>>24); p[5]=(uint8_t)(v>>16);
  p[6]=(uint8_t)(v>>8);  p[7]=(uint8_t)v;
}

static void init_hf_path(void) {
  const char *env = getenv("HF_PATH");
  if (env && *env) {
    snprintf(hf_path, sizeof(hf_path), "%s", env);
  } else {
    snprintf(hf_path, sizeof(hf_path), "./build/hf");
  }
}

static uint16_t alloc_port(void) {
  uint16_t p = next_port;
  next_port++;
  if (next_port > 29900) next_port = 19900;
  return p;
}

/* spawn hf; returns exit code. fills out/err buffers (null-terminated) */
static int spawn_hf(char *const argv[], char *out_buf,
                    char *err_buf) {
  int out_pipe[2], err_pipe[2];
  if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) return -1;

  pid_t pid = fork();
  if (pid < 0) return -1;

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
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* wait for TCP port to accept connections */
static int wait_port(uint16_t port, int timeout_ms) {
  int step = 50;
  int elapsed = 0;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  while (elapsed < timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      close(sock);
      return 0;
    }
    close(sock);
    usleep((useconds_t)step * 1000);
    elapsed += step;
  }
  return 1;
}

/* start hf recv in background; returns pid, waits for port ready */
static pid_t start_recv(const char *dir, uint16_t port) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    /* redirect stdout/stderr to /dev/null */
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      close(null);
    }
    /* discard stdin */
    close(STDIN_FILENO);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    execlp(hf_path, hf_path, "recv", dir, "-p", port_str, (char *)NULL);
    _exit(1);
  }
  /* wait for port to open */
  if (wait_port(port, 5000) != 0) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static void stop_recv(pid_t pid) {
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
}

/* connect to TCP and return fd, or -1 on failure */
static int tcp_connect(const char *host, uint16_t port) {
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

/* recv exact bytes from TCP, return total or -1 */
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

/* send full preamble (279 B) */
static int send_preamble(int sock, uint16_t magic, uint8_t ver,
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
  /* send_all from node.c not available here */
  size_t off = 0;
  while (off < sizeof(buf)) {
    ssize_t n = send(sock, buf + off, sizeof(buf) - off, 0);
    if (n < 0) { if (errno == EINTR) continue; return 1; }
    if (n == 0) return 1;
    off += (size_t)n;
  }
  return 0;
}

/* read response frame (4 B) */
static int recv_response(int sock, uint8_t *phase, uint8_t *status,
                          uint16_t *ec) {
  uint8_t buf[4];
  if (recv_exact(sock, buf, 4) != 4) return 1;
  *phase = buf[0];
  *status = buf[1];
  uint16_t e;
  memcpy(&e, buf + 2, 2);
  *ec = ntohs(e);
  return 0;
}

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

static int write_file(const char *path, const void *data, size_t len) {
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

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

static int files_equal(const char *a, const char *b) {
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

static int rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d) return 1;
    struct dirent *e;
    char sub[1024];
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
      rm_rf(sub);
    }
    closedir(d);
    rmdir(path);
  } else {
    unlink(path);
  }
  return 0;
}

static void make_tmp_path(char *buf, size_t cap, const char *name) {
  snprintf(buf, cap, "%s/%s", test_dir, name);
}

static int init_test_dir(void) {
  snprintf(test_dir, sizeof(test_dir), "/tmp/hf_int_test_XXXXXX");
  if (!mkdtemp(test_dir)) return 1;
  return 0;
}

static void cleanup_test_dir(void) {
  rm_rf(test_dir);
}

/* get count of .tmp.* files in a directory */
static int count_tmp_files(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  int count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strstr(e->d_name, ".tmp.")) count++;
  }
  closedir(d);
  return count;
}

/* ================================================================== */
/* CLI Tests                                                           */
/* ================================================================== */

TEST(cli_help) {
  char *argv[] = {hf_path, "-h", NULL};
  char out[OUT_CAP], err[OUT_CAP];
  int rc = spawn_hf(argv, out, err);
  ASSERT_EQ(rc, 0);
  ASSERT(strstr(out, "usage:") != NULL, "help prints usage to stdout");
  ASSERT(strstr(err, "usage:") == NULL, "help does not print usage to stderr");
}

TEST(cli_no_args) {
  char *argv[] = {hf_path, NULL};
  char out[OUT_CAP], err[OUT_CAP];
  int rc = spawn_hf(argv, out, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "missing command") != NULL, "reports missing command");
  ASSERT(strstr(err, "usage:") != NULL, "parse errors print usage to stderr");
  ASSERT(strstr(out, "usage:") == NULL, "parse errors do not print usage to stdout");
}

TEST(cli_unknown_cmd) {
  char *argv[] = {hf_path, "unknown", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "unknown command") != NULL, "reports unknown command");
}

TEST(cli_invalid_arg) {
  char *argv[] = {hf_path, "--foo", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "unknown command") != NULL, "--foo treated as unknown cmd");
}

TEST(cli_unknown_flag) {
  char *argv[] = {hf_path, "recv", "-x", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid argument") != NULL, "reports invalid flag");
}

TEST(cli_extra_arg) {
  char *argv[] = {hf_path, "recv", ".", "extra", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "unexpected extra argument") != NULL,
         "reports extra argument");
}

TEST(cli_port_not_number) {
  char *argv[] = {hf_path, "recv", "-p", "abc", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid port") != NULL, "reports invalid port");
}

TEST(cli_port_zero) {
  char *argv[] = {hf_path, "recv", "-p", "0", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid port") != NULL, "rejects port 0");
}

TEST(cli_port_overflow) {
  char *argv[] = {hf_path, "recv", "-p", "99999", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid port") != NULL, "rejects port > 65535");
}

TEST(cli_recv_rejects_i) {
  char *argv[] = {hf_path, "recv", "-i", "1.2.3.4", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "recv mode does not accept -i") != NULL,
         "recv rejects -i");
}

TEST(cli_send_no_file) {
  char *argv[] = {hf_path, "send", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "missing file") != NULL, "reports missing file");
}

TEST(cli_send_requires_i) {
  char src[512];
  make_tmp_path(src, sizeof(src), "requires_i.txt");
  write_file(src, "x", 1);

  char *argv[] = {hf_path, "send", src, NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "missing target address") != NULL,
         "send requires target address");
}

TEST(cli_send_nonexistent) {
  char *argv[] = {hf_path, "send", "/nonexistent/file_xyz", "-i", "127.0.0.1", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
}

TEST(cli_send_dir) {
  char *argv[] = {hf_path, "send", "/tmp", "-i", "127.0.0.1", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
}

/* ================================================================== */
/* Transfer Tests                                                      */
/* ================================================================== */

#define RECV_PORT_BASE 20100

static pid_t transfer_recv_pid = -1;
static uint16_t transfer_port = 0;
static char transfer_dir[512];

static int setup_transfer(void) {
  transfer_port = alloc_port() + 100;
  snprintf(transfer_dir, sizeof(transfer_dir), "%s/recv", test_dir);
  mkdir(transfer_dir, 0755);
  transfer_recv_pid = start_recv(transfer_dir, transfer_port);
  return transfer_recv_pid < 0 ? 1 : 0;
}

static void teardown_transfer(void) {
  if (transfer_recv_pid > 0) {
    stop_recv(transfer_recv_pid);
    transfer_recv_pid = -1;
  }
  rm_rf(transfer_dir);
}

TEST(transfer_common_file) {
  if (setup_transfer()) { ASSERT(0, "failed to start recv"); return; }

  char src[512];
  make_tmp_path(src, sizeof(src), "src.txt");
  write_file(src, "hello world\n", 12);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/src.txt", transfer_dir);
  ASSERT(file_exists(dst), "received file exists");
  ASSERT(files_equal(src, dst), "files match");

  teardown_transfer();
}

TEST(transfer_empty_file) {
  if (setup_transfer()) { ASSERT(0, "failed to start recv"); return; }

  char src[512];
  make_tmp_path(src, sizeof(src), "empty.txt");
  write_file(src, "", 0);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/empty.txt", transfer_dir);
  ASSERT(file_exists(dst), "empty file received");
  ASSERT_EQ((long long)file_size(dst), 0);

  teardown_transfer();
}

TEST(transfer_fixture_ascii) {
  if (setup_transfer()) { ASSERT(0, "failed to start recv"); return; }

  char src[512];
  snprintf(src, sizeof(src), "%s/test/fixtures/transfer/text_basic_ascii.txt",
           g_project_root);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/text_basic_ascii.txt", transfer_dir);
  ASSERT(file_exists(dst), "fixture received");
  ASSERT(files_equal(src, dst), "fixture content matches");

  teardown_transfer();
}

TEST(transfer_overwrite) {
  if (setup_transfer()) { ASSERT(0, "failed to start recv"); return; }

  char dir1[512], dir2[512];
  snprintf(dir1, sizeof(dir1), "%s/ow1", test_dir);
  snprintf(dir2, sizeof(dir2), "%s/ow2", test_dir);
  mkdir(dir1, 0755);
  mkdir(dir2, 0755);

  char src1[512], src2[512];
  snprintf(src1, sizeof(src1), "%s/ow.txt", dir1);
  snprintf(src2, sizeof(src2), "%s/ow.txt", dir2);
  write_file(src1, "first\n", 6);
  write_file(src2, "second data\n", 12);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_port);

  char *argv1[] = {hf_path, "send", src1, "-i", "127.0.0.1", "-p", port_str, NULL};
  ASSERT_EQ(spawn_hf(argv1, NULL, NULL), 0);

  /* overwrite — same filename from different dir */
  char *argv2[] = {hf_path, "send", src2, "-i", "127.0.0.1", "-p", port_str, NULL};
  ASSERT_EQ(spawn_hf(argv2, NULL, NULL), 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/ow.txt", transfer_dir);
  ASSERT(file_exists(dst), "overwrite file exists");
  ASSERT(files_equal(src2, dst), "overwrite content matches second file");

  rm_rf(dir1);
  rm_rf(dir2);
  teardown_transfer();
}

/* ================================================================== */
/* Protocol Tests                                                      */
/* ================================================================== */

static pid_t proto_recv_pid = -1;
static uint16_t proto_port = 0;
static char proto_dir[512];

static int setup_proto(void) {
  proto_port = alloc_port() + 200;
  snprintf(proto_dir, sizeof(proto_dir), "%s/proto_recv", test_dir);
  mkdir(proto_dir, 0755);
  proto_recv_pid = start_recv(proto_dir, proto_port);
  return proto_recv_pid < 0 ? 1 : 0;
}

static void teardown_proto(void) {
  if (proto_recv_pid > 0) {
    stop_recv(proto_recv_pid);
    proto_recv_pid = -1;
  }
  rm_rf(proto_dir);
}

TEST(proto_invalid_magic) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, 0x9999,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 5, "f.txt", 5, 5);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_invalid_version) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                0xFF, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a", 1, 3);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_invalid_msg_type) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, 0xFF, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a", 1, 3);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_zero_name_len) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 5, "", 0, 5);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_invalid_file_name) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a/b", 3, 3);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_payload_size_mismatch) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 100, "ok.txt", 3, 50);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
  teardown_proto();
}

TEST(proto_empty_body_transfer) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 0, "empty.txt", 9, 0);
  uint8_t ph, st; uint16_t ec;
  /* READY */
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_OK);
  /* no body */
  /* FINAL */
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(ph, PROTO_PHASE_FINAL);
  ASSERT_EQ(st, PROTO_STATUS_OK);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/empty.txt", proto_dir);
  ASSERT(file_exists(dst), "empty file created");
  ASSERT_EQ((long long)file_size(dst), 0);

  close(sock);
  teardown_proto();
}

TEST(proto_successful_transfer) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  const char *data = "hello protocol test\n";
  uint64_t dlen = (uint64_t)strlen(data);
  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + dlen, "pro.txt", 7, dlen);
  uint8_t ph, st; uint16_t ec;
  /* READY */
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_OK);
  /* send body */
  size_t off = 0;
  while (off < dlen) {
    ssize_t n = send(sock, data + off, dlen - off, 0);
    if (n < 0) { if (errno == EINTR) continue; break; }
    off += (size_t)n;
  }
  /* FINAL */
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(ph, PROTO_PHASE_FINAL);
  ASSERT_EQ(st, PROTO_STATUS_OK);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/pro.txt", proto_dir);
  ASSERT(file_exists(dst), "received file exists");

  char buf[256];
  int rd = read_file(dst, buf, sizeof(buf));
  ASSERT_EQ(rd, (int)dlen);
  buf[rd] = '\0';
  ASSERT_STREQ(buf, data);

  close(sock);
  teardown_proto();
}

TEST(proto_partial_body_cleanup) {
  if (setup_proto()) { ASSERT(0, "failed to start recv"); return; }
  int sock = tcp_connect("127.0.0.1", proto_port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_MSG_TYPE_SEND_FILE, HF_MSG_FLAG_NONE,
                2 + 256 + 8 + 100, "partial.bin", 10, 100);
  uint8_t ph, st; uint16_t ec;
  ASSERT_EQ(recv_response(sock, &ph, &st, &ec), 0);
  ASSERT_EQ(st, PROTO_STATUS_OK);

  /* send only part of body then close */
  char part[30] = {0};
  send(sock, part, 30, 0);
  close(sock);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/partial.bin", proto_dir);

  /* poll for cleanup — wait up to 3s for tmp files to disappear */
  for (int i = 0; i < 30; i++) {
    usleep(100 * 1000);
    if (count_tmp_files(proto_dir) == 0 && !file_exists(dst)) break;
  }

  /* file should not exist, no partial file either */
  ASSERT(!file_exists(dst), "partial file not present");
  ASSERT_EQ(count_tmp_files(proto_dir), 0);

  teardown_proto();
}

/* override hf_path via argv for users */
static const char *hf_override = NULL;

int main(int argc, char **argv) {
  if (argc > 1) hf_override = argv[1];

  init_hf_path();
  if (hf_override) snprintf(hf_path, sizeof(hf_path), "%s", hf_override);

  if (argc > 2)
    snprintf(g_project_root, sizeof(g_project_root), "%s", argv[2]);
  else
    snprintf(g_project_root, sizeof(g_project_root), ".");

  if (init_test_dir()) {
    fprintf(stderr, "failed to create test dir\n");
    return 1;
  }

  RUN_TESTS(
    T(cli_help),
    T(cli_no_args),
    T(cli_unknown_cmd),
    T(cli_invalid_arg),
    T(cli_unknown_flag),
    T(cli_extra_arg),
    T(cli_port_not_number),
    T(cli_port_zero),
    T(cli_port_overflow),
    T(cli_recv_rejects_i),
    T(cli_send_no_file),
    T(cli_send_requires_i),
    T(cli_send_nonexistent),
    T(cli_send_dir),
    T(transfer_common_file),
    T(transfer_empty_file),
    T(transfer_fixture_ascii),
    T(transfer_overwrite),
    T(proto_invalid_magic),
    T(proto_invalid_version),
    T(proto_invalid_msg_type),
    T(proto_zero_name_len),
    T(proto_invalid_file_name),
    T(proto_payload_size_mismatch),
    T(proto_empty_body_transfer),
    T(proto_successful_transfer),
    T(proto_partial_body_cleanup)
  );

  cleanup_test_dir();
  return _runner.failed ? 1 : 0;
}
