#include "integration_support.h"
#include "test.h"

#include <stdio.h>
#include <sys/stat.h>

static receiver_t transfer_receiver = RECEIVER_INIT;
static char transfer_dir[512];

static int setup_transfer(void) {
  snprintf(transfer_dir, sizeof(transfer_dir), "%s/recv", test_dir);
  mkdir(transfer_dir, 0755);
  return start_recv(transfer_dir, &transfer_receiver);
}

static void teardown_transfer(void) {
  stop_recv(&transfer_receiver);
  rm_rf(transfer_dir);
}

TEST(transfer_common_file) {
  char src[512];
  make_tmp_path(src, sizeof(src), "src.txt");
  write_file(src, "hello world\n", 12);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_receiver.port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/src.txt", transfer_dir);
  ASSERT(file_exists(dst), "received file exists");
  ASSERT(files_equal(src, dst), "files match");

}

TEST(transfer_empty_file) {
  char src[512];
  make_tmp_path(src, sizeof(src), "empty.txt");
  write_file(src, "", 0);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_receiver.port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/empty.txt", transfer_dir);
  ASSERT(file_exists(dst), "empty file received");
  ASSERT_EQ((long long)file_size(dst), 0);

}

TEST(transfer_fixture_ascii) {
  char src[512];
  snprintf(src, sizeof(src), "%s/test/fixtures/transfer/text_basic_ascii.txt",
           g_project_root);

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_receiver.port);
  char *argv[] = {hf_path, "send", src, "-i", "127.0.0.1", "-p", port_str, NULL};
  int rc = spawn_hf(argv, NULL, NULL);
  ASSERT_EQ(rc, 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/text_basic_ascii.txt", transfer_dir);
  ASSERT(file_exists(dst), "fixture received");
  ASSERT(files_equal(src, dst), "fixture content matches");

}

TEST(transfer_overwrite) {
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
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)transfer_receiver.port);

  char *argv1[] = {hf_path, "send", src1, "-i", "127.0.0.1", "-p", port_str, NULL};
  ASSERT_EQ(spawn_hf(argv1, NULL, NULL), 0);

  char *argv2[] = {hf_path, "send", src2, "-i", "127.0.0.1", "-p", port_str, NULL};
  ASSERT_EQ(spawn_hf(argv2, NULL, NULL), 0);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/ow.txt", transfer_dir);
  ASSERT(file_exists(dst), "overwrite file exists");
  ASSERT(files_equal(src2, dst), "overwrite content matches second file");

  rm_rf(dir1);
  rm_rf(dir2);
}

int main(int argc, char **argv) {
  if (integration_init(argc, argv)) return 1;

  RUN_TESTS_WITH_FIXTURE(setup_transfer, teardown_transfer,
    T(transfer_common_file),
    T(transfer_empty_file),
    T(transfer_fixture_ascii),
    T(transfer_overwrite)
  );

  integration_cleanup();
  return _runner.failed ? 1 : 0;
}
