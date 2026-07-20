#include "integration_support.h"
#include "protocol.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static receiver_t proto_receiver = RECEIVER_INIT;
static char proto_dir[512];

static int setup_proto(void) {
  snprintf(proto_dir, sizeof(proto_dir), "%s/proto_recv", test_dir);
  mkdir(proto_dir, 0755);
  return start_recv(proto_dir, &proto_receiver);
}

static void teardown_proto(void) {
  stop_recv(&proto_receiver);
  rm_rf(proto_dir);
}

TEST(proto_invalid_magic) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, 0x9999,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 5, "f.txt", 5, 5);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_invalid_version) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                0xFF, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a", 1, 3);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_invalid_msg_type) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, 0xFF, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a", 1, 3);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_zero_name_len) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 5, "", 0, 5);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_invalid_file_name) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 3, "a/b", 3, 3);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_payload_size_mismatch) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 100, "ok.txt", 3, 50);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_REJECTED);
  close(sock);
}

TEST(proto_empty_body_transfer) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 0, "empty.txt", 9, 0);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_OK);
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(ph, PROTO_PHASE_FINAL);
  ASSERT_EQ(st, PROTO_STATUS_OK);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/empty.txt", proto_dir);
  ASSERT(file_exists(dst), "empty file created");
  ASSERT_EQ((long long)file_size(dst), 0);

  close(sock);
}

TEST(proto_successful_transfer) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  const char *data = "hello protocol test\n";
  uint64_t dlen = (uint64_t)strlen(data);
  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + dlen, "pro.txt", 7, dlen);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(ph, PROTO_PHASE_READY);
  ASSERT_EQ(st, PROTO_STATUS_OK);
  size_t off = 0;
  while (off < dlen) {
    ssize_t n = send(sock, data + off, dlen - off, 0);
    if (n < 0) { if (errno == EINTR) continue; break; }
    off += (size_t)n;
  }
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
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
}

TEST(proto_partial_body_cleanup) {
  int sock = tcp_connect("127.0.0.1", proto_receiver.port);
  ASSERT(sock >= 0, "connect ok");

  send_preamble(sock, HF_PROTOCOL_MAGIC,
                HF_PROTOCOL_VERSION, HF_PROTOCOL_MSG_TYPE_SEND_FILE, HF_PROTOCOL_MSG_FLAG_NONE,
                2 + 256 + 8 + 100, "partial.bin", 10, 100);
  uint8_t ph, st;
  ASSERT_EQ(recv_response(sock, &ph, &st), 0);
  ASSERT_EQ(st, PROTO_STATUS_OK);

  char part[30] = {0};
  send(sock, part, 30, 0);
  close(sock);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/partial.bin", proto_dir);

  for (int i = 0; i < 30; i++) {
    usleep(100 * 1000);
    if (count_tmp_files(proto_dir) == 0 && !file_exists(dst)) break;
  }

  ASSERT(!file_exists(dst), "partial file not present");
  ASSERT_EQ(count_tmp_files(proto_dir), 0);

}

int main(int argc, char **argv) {
  if (integration_init(argc, argv)) return 1;

  RUN_TESTS_WITH_FIXTURE(setup_proto, teardown_proto,
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

  integration_cleanup();
  return _runner.failed ? 1 : 0;
}
