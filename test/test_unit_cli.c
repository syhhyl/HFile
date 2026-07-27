#include "test.h"

#define HF_CLI_NO_MAIN
#include "../src/hfile.c"
#undef HF_CLI_NO_MAIN

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

TEST(cli_parse_recv_rejects_attached_port) {
  char *argv[] = {"hf", "recv", "-p19999", NULL};
  CliArgs args;

  ASSERT_NE(parse_cli(3, argv, &args), 0);
  ASSERT_STREQ(args.error, "invalid argument");
}

TEST(cli_parse_recv_rejects_option_terminator) {
  char *argv[] = {"hf", "recv", "--", NULL};
  CliArgs args;

  ASSERT_NE(parse_cli(3, argv, &args), 0);
  ASSERT_STREQ(args.error, "invalid argument");
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

TEST(cli_parse_send_rejects_attached_ip) {
  char *argv[] = {"hf", "send", "file.txt", "-i127.0.0.1", NULL};
  CliArgs args;

  ASSERT_NE(parse_cli(4, argv, &args), 0);
  ASSERT_STREQ(args.error, "invalid argument order");
}

int main(void) {
  RUN_TESTS(
    T(cli_parse_recv_defaults),
    T(cli_parse_recv_port_without_dir),
    T(cli_parse_recv_dir_then_port),
    T(cli_parse_recv_rejects_dir_after_port),
    T(cli_parse_recv_rejects_attached_port),
    T(cli_parse_recv_rejects_option_terminator),
    T(cli_parse_send_default_port),
    T(cli_parse_send_file_ip_port_order),
    T(cli_parse_send_rejects_port_before_ip),
    T(cli_parse_send_rejects_attached_ip)
  );
  return _runner.failed ? 1 : 0;
}
