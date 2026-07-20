#include "test.h"

#include <sys/socket.h>
#include <unistd.h>

#include "../src/net.c"

TEST(socket_invalid_negative) {
  ASSERT(is_socket_invalid(-1), "-1 is invalid");
}

TEST(socket_valid_zero) {
  ASSERT(!is_socket_invalid(0), "0 is valid");
}

TEST(socket_valid_positive) {
  ASSERT(!is_socket_invalid(5), "5 is valid");
}

TEST(send_all_zero_len) {
  ssize_t result = send_all(-1, "x", 0);
  ASSERT_EQ((long long)result, 0);
}

TEST(recv_all_zero_len) {
  ssize_t result = recv_all(-1, NULL, 0);
  ASSERT_EQ((long long)result, 0);
}

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
  ssize_t result = recv_all(sv[1], buf, 5);
  ASSERT_EQ((long long)result, 2);
  ASSERT_EQ(buf[0], 'a');
  ASSERT_EQ(buf[1], 'b');

  close(sv[1]);
}

int main(void) {
  RUN_TESTS(
    T(socket_invalid_negative),
    T(socket_valid_zero),
    T(socket_valid_positive),
    T(send_all_zero_len),
    T(recv_all_zero_len),
    T(send_all_recv_all_roundtrip),
    T(recv_all_partial)
  );
  return _runner.failed ? 1 : 0;
}
