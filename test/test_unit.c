#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#include "../src/net.c"
#include "../src/node.c"

#define HF_CLI_NO_MAIN
#include "../src/hfile.c"
#undef HF_CLI_NO_MAIN

/* --- CLI parsing --- */

TEST(cli_parse_recv_defaults) {
  char *argv[] = {"hf", "recv", NULL};
  CliArgs args;

  ASSERT_EQ(parse_cli(2, argv, &args), 0);
  ASSERT_EQ(args.mode, MODE_RECV);
  ASSERT_STREQ(args.path, ".");
  ASSERT_EQ(args.port, DEFAULT_PORT);
  ASSERT_NULL(args.ip);
}

TEST(cli_parse_recv_port_without_dir) {
  char *argv[] = {"hf", "recv", "-p", "19999", NULL};
  CliArgs args;

  ASSERT_EQ(parse_cli(4, argv, &args), 0);
  ASSERT_EQ(args.mode, MODE_RECV);
  ASSERT_STREQ(args.path, ".");
  ASSERT_EQ(args.port, 19999);
}

TEST(cli_parse_recv_dir_then_port) {
  char *argv[] = {"hf", "recv", "downloads", "-p", "19999", NULL};
  CliArgs args;

  ASSERT_EQ(parse_cli(5, argv, &args), 0);
  ASSERT_EQ(args.mode, MODE_RECV);
  ASSERT_STREQ(args.path, "downloads");
  ASSERT_EQ(args.port, 19999);
}

TEST(cli_parse_recv_rejects_dir_after_port) {
  char *argv[] = {"hf", "recv", "-p", "19999", "downloads", NULL};
  CliArgs args;

  ASSERT_NE(parse_cli(5, argv, &args), 0);
  ASSERT_STREQ(args.error, "unexpected extra argument");
}

TEST(cli_parse_send_default_port) {
  char *argv[] = {"hf", "send", "file.txt", "-i", "127.0.0.1", NULL};
  CliArgs args;

  ASSERT_EQ(parse_cli(5, argv, &args), 0);
  ASSERT_EQ(args.mode, MODE_SEND);
  ASSERT_STREQ(args.path, "file.txt");
  ASSERT_STREQ(args.ip, "127.0.0.1");
  ASSERT_EQ(args.port, DEFAULT_PORT);
}

TEST(cli_parse_send_file_ip_port_order) {
  char *argv[] = {
    "hf", "send", "file.txt", "-i", "127.0.0.1", "-p", "19999", NULL
  };
  CliArgs args;

  ASSERT_EQ(parse_cli(7, argv, &args), 0);
  ASSERT_EQ(args.mode, MODE_SEND);
  ASSERT_STREQ(args.path, "file.txt");
  ASSERT_STREQ(args.ip, "127.0.0.1");
  ASSERT_EQ(args.port, 19999);
}

TEST(cli_parse_send_rejects_port_before_ip) {
  char *argv[] = {
    "hf", "send", "file.txt", "-p", "19999", "-i", "127.0.0.1", NULL
  };
  CliArgs args;

  ASSERT_NE(parse_cli(7, argv, &args), 0);
  ASSERT_STREQ(args.error, "invalid argument order");
}

/* --- be64_read / be64_write --- */

TEST(be64_zero) {
  uint8_t buf[] = {0,0,0,0,0,0,0,0};
  ASSERT_EQ((long long)be64_read(buf), 0);
}

TEST(be64_max) {
  uint8_t buf[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  ASSERT_EQ((long long)be64_read(buf), (long long)UINT64_MAX);
}

TEST(be64_one) {
  uint8_t buf[] = {0,0,0,0,0,0,0,1};
  ASSERT_EQ((long long)be64_read(buf), 1);
}

TEST(be64_roundtrip) {
  uint8_t buf[8];
  uint64_t v = 0xDEADBEEFCAFE0001ULL;
  be64_write(buf, v);
  ASSERT_EQ((long long)be64_read(buf), (long long)v);
}

TEST(be64_roundtrip_zero) {
  uint8_t buf[8];
  be64_write(buf, 0);
  ASSERT_EQ((long long)be64_read(buf), 0);
}

TEST(be64_roundtrip_max) {
  uint8_t buf[8];
  be64_write(buf, UINT64_MAX);
  ASSERT_EQ((long long)be64_read(buf), (long long)UINT64_MAX);
}

TEST(be64_byte_order) {
  uint8_t buf[8];
  be64_write(buf, 0x0102030405060708ULL);
  ASSERT_EQ(buf[0], 0x01);
  ASSERT_EQ(buf[1], 0x02);
  ASSERT_EQ(buf[2], 0x03);
  ASSERT_EQ(buf[3], 0x04);
  ASSERT_EQ(buf[4], 0x05);
  ASSERT_EQ(buf[5], 0x06);
  ASSERT_EQ(buf[6], 0x07);
  ASSERT_EQ(buf[7], 0x08);
}

/* --- ok_name --- */

TEST(ok_name_null) {
  ASSERT(!ok_name(NULL), "null name invalid");
}

TEST(ok_name_empty) {
  ASSERT(!ok_name(""), "empty name invalid");
}

TEST(ok_name_normal) {
  ASSERT(ok_name("hello.txt"), "normal name valid");
}

TEST(ok_name_dot_only) {
  ASSERT(ok_name("."), "dot only valid");
}

TEST(ok_name_dot_file) {
  ASSERT(ok_name(".hidden"), "dot file valid");
}

TEST(ok_name_with_slash) {
  ASSERT(!ok_name("a/b"), "slash rejected");
}

TEST(ok_name_with_backslash) {
  ASSERT(!ok_name("a\\b"), "backslash rejected");
}

TEST(ok_name_dotdot) {
  ASSERT(!ok_name(".."), "dotdot rejected");
}

TEST(ok_name_dotdot_file) {
  ASSERT(!ok_name("a..b"), "dotdot in filename rejected (uses strstr)");
}

TEST(ok_name_dotdot_path) {
  ASSERT(!ok_name("foo/../bar"), "dotdot path rejected");
}

/* --- join_path --- */

TEST(join_path_trailing_slash) {
  char buf[256];
  int overflow = join_path(buf, sizeof(buf), "/tmp/", "file.txt");
  ASSERT(!overflow, "no overflow expected");
  ASSERT_STREQ(buf, "/tmp/file.txt");
}

TEST(join_path_no_trailing_slash) {
  char buf[256];
  int overflow = join_path(buf, sizeof(buf), "/tmp", "file.txt");
  ASSERT(!overflow, "no overflow expected");
  ASSERT_STREQ(buf, "/tmp/file.txt");
}

TEST(join_path_overflow) {
  char buf[5];
  int overflow = join_path(buf, sizeof(buf), "/a", "bc");
  ASSERT(overflow, "overflow expected");
}

TEST(join_path_exact_fit) {
  char buf[12];
  int overflow = join_path(buf, sizeof(buf), "/a", "bc");
  ASSERT(!overflow, "no overflow expected");
  ASSERT_STREQ(buf, "/a/bc");
}

/* --- tmp_path --- */

TEST(tmp_path_format) {
  char buf[256];
  int overflow = tmp_path(buf, sizeof(buf), "/tmp/file.txt", 12345, 3);
  ASSERT(!overflow, "no overflow expected");
  ASSERT_STREQ(buf, "/tmp/file.txt.tmp.12345.3");
}

TEST(tmp_path_overflow) {
  char buf[10];
  int overflow = tmp_path(buf, sizeof(buf), "/tmp/file.txt", 12345, 3);
  ASSERT(overflow, "overflow expected");
}

/* --- reply validation --- */

TEST(reply_invalid_socket) {
  int rc = reply(-1, PROTO_PHASE_READY, PROTO_STATUS_OK);
  ASSERT_NE(rc, 0);
}

TEST(reply_invalid_phase) {
  int rc = reply(1, 2, PROTO_STATUS_OK);
  ASSERT_NE(rc, 0);
}

TEST(reply_invalid_status) {
  int rc = reply(1, PROTO_PHASE_READY, 3);
  ASSERT_NE(rc, 0);
}

/* --- is_socket_invalid --- */

TEST(socket_invalid_negative) {
  ASSERT(is_socket_invalid(-1), "-1 is invalid");
}

TEST(socket_valid_zero) {
  ASSERT(!is_socket_invalid(0), "0 is valid");
}

TEST(socket_valid_positive) {
  ASSERT(!is_socket_invalid(5), "5 is valid");
}

/* --- send_all / recv_all zero-length --- */

TEST(send_all_zero_len) {
  ssize_t r = send_all(-1, "x", 0);
  ASSERT_EQ((long long)r, 0);
}

TEST(recv_all_zero_len) {
  ssize_t r = recv_all(-1, NULL, 0);
  ASSERT_EQ((long long)r, 0);
}

/* --- reply wire format via socketpair --- */

TEST(reply_frame_format) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  int rc = reply(sv[0], PROTO_PHASE_READY, PROTO_STATUS_OK);
  ASSERT_EQ(rc, 0);

  uint8_t buf[2];
  ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
  ASSERT_EQ((long long)n, (long long)sizeof(buf));
  ASSERT_EQ(buf[0], PROTO_PHASE_READY);
  ASSERT_EQ(buf[1], PROTO_STATUS_OK);

  close(sv[0]);
  close(sv[1]);
}

TEST(reply_frame_format_rejected) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  int rc = reply(sv[0], PROTO_PHASE_READY, PROTO_STATUS_REJECTED);
  ASSERT_EQ(rc, 0);
  uint8_t buf[2];
  ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
  ASSERT_EQ((long long)n, (long long)sizeof(buf));
  ASSERT_EQ(buf[0], PROTO_PHASE_READY);
  ASSERT_EQ(buf[1], PROTO_STATUS_REJECTED);

  close(sv[0]);
  close(sv[1]);
}

/* --- send_all / recv_all via socketpair --- */

TEST(send_all_recv_all_roundtrip) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  const char *msg = "hello world";
  size_t len = strlen(msg);
  ssize_t sent = send_all(sv[0], msg, len);
  ASSERT_EQ((long long)sent, (long long)len);

  char buf[64] = {0};
  ssize_t recvd = recv_all(sv[1], buf, len);
  ASSERT_EQ((long long)recvd, (long long)len);
  ASSERT_STREQ(buf, msg);

  close(sv[0]);
  close(sv[1]);
}

TEST(recv_all_partial) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  send(sv[0], "ab", 2, 0);
  close(sv[0]);

  char buf[64] = {0};
  ssize_t r = recv_all(sv[1], buf, 5);
  ASSERT_EQ((long long)r, 2);
  ASSERT_EQ(buf[0], 'a');
  ASSERT_EQ(buf[1], 'b');

  close(sv[1]);
}

int main(void) {
  RUN_TESTS(
    T(cli_parse_recv_defaults),
    T(cli_parse_recv_port_without_dir),
    T(cli_parse_recv_dir_then_port),
    T(cli_parse_recv_rejects_dir_after_port),
    T(cli_parse_send_default_port),
    T(cli_parse_send_file_ip_port_order),
    T(cli_parse_send_rejects_port_before_ip),
    T(be64_zero),
    T(be64_max),
    T(be64_one),
    T(be64_roundtrip),
    T(be64_roundtrip_zero),
    T(be64_roundtrip_max),
    T(be64_byte_order),
    T(ok_name_null),
    T(ok_name_empty),
    T(ok_name_normal),
    T(ok_name_dot_only),
    T(ok_name_dot_file),
    T(ok_name_with_slash),
    T(ok_name_with_backslash),
    T(ok_name_dotdot),
    T(ok_name_dotdot_file),
    T(ok_name_dotdot_path),
    T(join_path_trailing_slash),
    T(join_path_no_trailing_slash),
    T(join_path_overflow),
    T(join_path_exact_fit),
    T(tmp_path_format),
    T(tmp_path_overflow),
    T(reply_invalid_socket),
    T(reply_invalid_phase),
    T(reply_invalid_status),
    T(socket_invalid_negative),
    T(socket_valid_zero),
    T(socket_valid_positive),
    T(send_all_zero_len),
    T(recv_all_zero_len),
    T(reply_frame_format),
    T(reply_frame_format_rejected),
    T(send_all_recv_all_roundtrip),
    T(recv_all_partial)
  );
  return _runner.failed ? 1 : 0;
}
