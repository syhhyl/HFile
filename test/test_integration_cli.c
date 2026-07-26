#include "integration_support.h"
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

TEST(cli_recv_rejects_missing_dir) {
  char missing[512];
  snprintf(missing, sizeof(missing), "%s/missing-recv-dir", test_dir);

  char *argv[] = {hf_path, "recv", missing, NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid receive directory") != NULL,
         "reports invalid receive directory");
}

TEST(cli_recv_rejects_occupied_port) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT(listener >= 0, "create listener");

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(0);
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    close(listener);
    ASSERT(0, "reserve listener port");
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(listener);
    ASSERT(0, "read listener port");
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)ntohs(addr.sin_port));
  char *argv[] = {hf_path, "recv", test_dir, "-p", port_str, NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  close(listener);

  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "failed to start receiver") != NULL,
         "reports occupied port");
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

TEST(cli_send_rejects_invalid_i) {
  char src[512];
  make_tmp_path(src, sizeof(src), "invalid_i.txt");
  write_file(src, "x", 1);

  char *argv[] = {hf_path, "send", src, "-i", "syh", NULL};
  char err[OUT_CAP];
  int rc = spawn_hf(argv, NULL, err);
  ASSERT_NE(rc, 0);
  ASSERT(strstr(err, "invalid address") != NULL,
         "send rejects invalid target address");
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

int main(int argc, char **argv) {
  if (integration_init(argc, argv)) return 1;

  RUN_TESTS(
    T(cli_help),
    T(cli_no_args),
    T(cli_unknown_cmd),
    T(cli_unknown_flag),
    T(cli_extra_arg),
    T(cli_port_not_number),
    T(cli_port_zero),
    T(cli_port_overflow),
    T(cli_recv_rejects_i),
    T(cli_recv_rejects_missing_dir),
    T(cli_recv_rejects_occupied_port),
    T(cli_send_no_file),
    T(cli_send_requires_i),
    T(cli_send_rejects_invalid_i),
    T(cli_send_nonexistent),
    T(cli_send_dir)
  );

  integration_cleanup();
  return _runner.failed ? 1 : 0;
}
