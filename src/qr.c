#include "qr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QR_VERSION 3
#define QR_SIZE 29
#define QR_DATA_CODEWORDS 55
#define QR_ECC_CODEWORDS 15
#define QR_TOTAL_CODEWORDS (QR_DATA_CODEWORDS + QR_ECC_CODEWORDS)

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready = 0;

typedef struct {
  uint8_t data[QR_DATA_CODEWORDS];
  size_t bit_len;
} qr_bits_t;

static void gf_init(void) {
  if (gf_ready) return;

  uint16_t x = 1;
  for (int i = 0; i < 255; i++) {
    gf_exp[i] = (uint8_t)x;
    gf_log[x] = (uint8_t)i;
    x <<= 1;
    if (x & 0x100u) x ^= 0x11Du;
  }
  for (int i = 255; i < 512; i++) {
    gf_exp[i] = gf_exp[i - 255];
  }

  gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) return 0;
  return gf_exp[gf_log[a] + gf_log[b]];
}

static void rs_generator(uint8_t gen[QR_ECC_CODEWORDS + 1]) {
  memset(gen, 0, QR_ECC_CODEWORDS + 1);
  gen[0] = 1;

  for (int i = 0; i < QR_ECC_CODEWORDS; i++) {
    uint8_t root = gf_exp[i];
    for (int j = i; j >= 0; j--) {
      gen[j + 1] ^= gf_mul(gen[j], root);
    }
  }
}

static void rs_compute(const uint8_t data[QR_DATA_CODEWORDS],
                       uint8_t ecc[QR_ECC_CODEWORDS]) {
  uint8_t gen[QR_ECC_CODEWORDS + 1];
  rs_generator(gen);
  memset(ecc, 0, QR_ECC_CODEWORDS);

  for (int i = 0; i < QR_DATA_CODEWORDS; i++) {
    uint8_t factor = data[i] ^ ecc[0];
    memmove(&ecc[0], &ecc[1], QR_ECC_CODEWORDS - 1);
    ecc[QR_ECC_CODEWORDS - 1] = 0;
    for (int j = 0; j < QR_ECC_CODEWORDS; j++) {
      ecc[j] ^= gf_mul(gen[j + 1], factor);
    }
  }
}

static int bits_append(qr_bits_t *bits, uint32_t value, int count) {
  if (bits == NULL || count < 0) return 1;
  if (bits->bit_len + (size_t)count > QR_DATA_CODEWORDS * 8u) return 1;

  for (int i = count - 1; i >= 0; i--) {
    size_t bit = bits->bit_len++;
    if ((value >> i) & 1u) {
      bits->data[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
    }
  }

  return 0;
}

static int encode_data(const char *url, uint8_t out[QR_TOTAL_CODEWORDS]) {
  size_t len = strlen(url);
  if (len > 40u) return 1;

  qr_bits_t bits;
  memset(&bits, 0, sizeof(bits));

  if (bits_append(&bits, 0x4u, 4) != 0) return 1;
  if (bits_append(&bits, (uint32_t)len, 8) != 0) return 1;
  for (size_t i = 0; i < len; i++) {
    if (bits_append(&bits, (uint8_t)url[i], 8) != 0) return 1;
  }

  size_t remaining = QR_DATA_CODEWORDS * 8u - bits.bit_len;
  if (bits_append(&bits, 0, remaining < 4u ? (int)remaining : 4) != 0) return 1;
  while ((bits.bit_len % 8u) != 0) {
    if (bits_append(&bits, 0, 1) != 0) return 1;
  }

  for (size_t i = bits.bit_len / 8u; i < QR_DATA_CODEWORDS; i++) {
    bits.data[i] = (i % 2u) == 0 ? 0xECu : 0x11u;
  }

  memcpy(out, bits.data, QR_DATA_CODEWORDS);
  gf_init();
  rs_compute(bits.data, out + QR_DATA_CODEWORDS);
  return 0;
}

static void set_module(uint8_t qr[QR_SIZE][QR_SIZE],
                       uint8_t reserved[QR_SIZE][QR_SIZE],
                       int x, int y, int dark, int reserve) {
  if (x < 0 || y < 0 || x >= QR_SIZE || y >= QR_SIZE) return;
  qr[y][x] = dark ? 1u : 0u;
  if (reserve) reserved[y][x] = 1u;
}

static void draw_finder(uint8_t qr[QR_SIZE][QR_SIZE],
                        uint8_t reserved[QR_SIZE][QR_SIZE], int x, int y) {
  for (int dy = -1; dy <= 7; dy++) {
    for (int dx = -1; dx <= 7; dx++) {
      int xx = x + dx;
      int yy = y + dy;
      if (xx < 0 || yy < 0 || xx >= QR_SIZE || yy >= QR_SIZE) continue;

      int dark = 0;
      if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) {
        dark = dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
               (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
      }
      set_module(qr, reserved, xx, yy, dark, 1);
    }
  }
}

static void draw_alignment(uint8_t qr[QR_SIZE][QR_SIZE],
                           uint8_t reserved[QR_SIZE][QR_SIZE], int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      int d = dx < 0 ? -dx : dx;
      int e = dy < 0 ? -dy : dy;
      int dark = d == 2 || e == 2 || (dx == 0 && dy == 0);
      set_module(qr, reserved, cx + dx, cy + dy, dark, 1);
    }
  }
}

static uint16_t format_bits(void) {
  uint32_t data = 0x08u;  // ECC level L, mask 0.
  uint32_t rem = data << 10;
  for (int i = 14; i >= 10; i--) {
    if ((rem >> i) & 1u) rem ^= 0x537u << (i - 10);
  }
  return (uint16_t)(((data << 10) | rem) ^ 0x5412u);
}

static void draw_format(uint8_t qr[QR_SIZE][QR_SIZE],
                        uint8_t reserved[QR_SIZE][QR_SIZE]) {
  uint16_t bits = format_bits();

  for (int i = 0; i <= 5; i++) set_module(qr, reserved, 8, i, (bits >> i) & 1u, 1);
  set_module(qr, reserved, 8, 7, (bits >> 6) & 1u, 1);
  set_module(qr, reserved, 8, 8, (bits >> 7) & 1u, 1);
  set_module(qr, reserved, 7, 8, (bits >> 8) & 1u, 1);
  for (int i = 9; i < 15; i++) set_module(qr, reserved, 14 - i, 8, (bits >> i) & 1u, 1);

  for (int i = 0; i < 8; i++) {
    set_module(qr, reserved, QR_SIZE - 1 - i, 8, (bits >> i) & 1u, 1);
  }
  for (int i = 8; i < 15; i++) {
    set_module(qr, reserved, 8, QR_SIZE - 15 + i, (bits >> i) & 1u, 1);
  }
}

static void draw_function_modules(uint8_t qr[QR_SIZE][QR_SIZE],
                                  uint8_t reserved[QR_SIZE][QR_SIZE]) {
  draw_finder(qr, reserved, 0, 0);
  draw_finder(qr, reserved, QR_SIZE - 7, 0);
  draw_finder(qr, reserved, 0, QR_SIZE - 7);

  for (int i = 8; i < QR_SIZE - 8; i++) {
    int dark = (i % 2) == 0;
    set_module(qr, reserved, i, 6, dark, 1);
    set_module(qr, reserved, 6, i, dark, 1);
  }

  draw_alignment(qr, reserved, 22, 22);
  set_module(qr, reserved, 8, 4 * QR_VERSION + 9, 1, 1);
  draw_format(qr, reserved);
}

static void place_data(uint8_t qr[QR_SIZE][QR_SIZE],
                       uint8_t reserved[QR_SIZE][QR_SIZE],
                       const uint8_t data[QR_TOTAL_CODEWORDS]) {
  size_t bit_index = 0;
  int upward = 1;

  for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
    if (right == 6) right--;

    for (int vert = 0; vert < QR_SIZE; vert++) {
      int y = upward ? QR_SIZE - 1 - vert : vert;
      for (int dx = 0; dx < 2; dx++) {
        int x = right - dx;
        if (reserved[y][x]) continue;

        int dark = 0;
        if (bit_index < QR_TOTAL_CODEWORDS * 8u) {
          dark = (data[bit_index / 8u] >> (7u - (bit_index % 8u))) & 1u;
          bit_index++;
        }
        if (((x + y) % 2) == 0) dark = !dark;
        set_module(qr, reserved, x, y, dark, 0);
      }
    }

    upward = !upward;
  }
}

static int make_qr(const char *url, uint8_t qr[QR_SIZE][QR_SIZE]) {
  uint8_t reserved[QR_SIZE][QR_SIZE];
  uint8_t data[QR_TOTAL_CODEWORDS];

  if (url == NULL) return 1;
  if (encode_data(url, data) != 0) return 1;

  memset(qr, 0, QR_SIZE * QR_SIZE);
  memset(reserved, 0, QR_SIZE * QR_SIZE);
  draw_function_modules(qr, reserved);
  place_data(qr, reserved, data);
  return 0;
}

int qr_print_url(const char *url, FILE *out) {
  uint8_t qr[QR_SIZE][QR_SIZE];
  FILE *dest = out == NULL ? stdout : out;

  if (make_qr(url, qr) != 0) return 1;

  for (int y = -2; y < QR_SIZE + 2; y += 2) {
    for (int x = -2; x < QR_SIZE + 2; x++) {
      int upper = x >= 0 && y >= 0 && x < QR_SIZE && y < QR_SIZE && qr[y][x];
      int lower = x >= 0 && y + 1 >= 0 && x < QR_SIZE && y + 1 < QR_SIZE &&
                  qr[y + 1][x];
      if (upper && lower) {
        fputs("█", dest);
      } else if (upper) {
        fputs("▀", dest);
      } else if (lower) {
        fputs("▄", dest);
      } else {
        fputc(' ', dest);
      }
    }
    fputc('\n', dest);
  }

  return ferror(dest) ? 1 : 0;
}
