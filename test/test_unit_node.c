#include "test.h"

#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/net.c"
#include "../src/node.c"

TEST(be64_roundtrip) {
  uint8_t buf[8];
  uint64_t v = 0xDEADBEEFCAFE0001ULL;
  be64_write(buf, v);
  ASSERT_EQ((long long)be64_read(buf), (long long)v);
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

TEST(ok_name_normal) {
  ASSERT(ok_name("hello.txt"), "normal name valid");
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

TEST(tmp_path_format) {
  char buf[256];
  int overflow = tmp_path(buf, sizeof(buf), "/tmp/file.txt", 12345, 3);
  ASSERT(!overflow, "no overflow expected");
  ASSERT_STREQ(buf, "/tmp/file.txt.tmp.12345.3");
}

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

int main(void) {
  RUN_TESTS(
    T(be64_roundtrip),
    T(be64_byte_order),
    T(ok_name_normal),
    T(ok_name_dot_file),
    T(ok_name_with_slash),
    T(ok_name_with_backslash),
    T(ok_name_dotdot),
    T(ok_name_dotdot_file),
    T(join_path_trailing_slash),
    T(join_path_no_trailing_slash),
    T(join_path_overflow),
    T(tmp_path_format),
    T(reply_frame_format)
  );
  return _runner.failed ? 1 : 0;
}
