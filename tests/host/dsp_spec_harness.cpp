// Host harness for tests/test_dsp_spec.py: audio/dsp_spec.cpp as it builds for the board.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/dsp_spec.h"

using namespace esphome::airplay_receiver;

static int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
  AirPlayDspFilter f[AIRPLAY_DSP_MAX_FILTERS];
  size_t n = 99;

  CHECK(dsp_spec_parse("hp,80,0.71,0;ls,150,0.7,1.5;pk,1281,1.84,-5.6", f, AIRPLAY_DSP_MAX_FILTERS, &n));
  CHECK(n == 3);
  CHECK(f[0].type == AIRPLAY_DSP_HIGH_PASS && near(f[0].frequency_hz, 80) && near(f[0].q, 0.71f));
  CHECK(f[1].type == AIRPLAY_DSP_LOW_SHELF && near(f[1].gain_db, 1.5f));
  CHECK(f[2].type == AIRPLAY_DSP_PEAKING && near(f[2].frequency_hz, 1281) && near(f[2].gain_db, -5.6f));

  char text[256];
  size_t len = dsp_spec_format(f, n, text, sizeof(text));
  CHECK(len == strlen(text));
  CHECK(strcmp(text, "hp,80,0.71,0;ls,150,0.70,1.5;pk,1281,1.84,-5.6") == 0);

  AirPlayDspFilter back[AIRPLAY_DSP_MAX_FILTERS];
  size_t m = 0;
  CHECK(dsp_spec_parse(text, back, AIRPLAY_DSP_MAX_FILTERS, &m) && m == n);

  CHECK(dsp_spec_parse("hs,3000,0.7,3;lp,12000,0.71,0;no,500,4,0", f, AIRPLAY_DSP_MAX_FILTERS, &n) && n == 3);
  CHECK(f[0].type == AIRPLAY_DSP_HIGH_SHELF && f[1].type == AIRPLAY_DSP_LOW_PASS && f[2].type == AIRPLAY_DSP_NOTCH);

  CHECK(dsp_spec_parse("flat", f, AIRPLAY_DSP_MAX_FILTERS, &n) && n == 0);
  CHECK(dsp_spec_format(f, 0, text, sizeof(text)) == 4 && strcmp(text, "flat") == 0);

  // Rejected without touching the output.
  n = 7;
  f[0].frequency_hz = 1234;
  const char *bad[] = {"", "xx,100,1,0", "pk,100,1", "pk,100,1,0,9", "pk,abc,1,0", "pk,100,1,0;", "pk,5,1,0",
                       "pk,100,0.01,0", "pk,100,1,30", "pk,1e9,1,0", "pk,100,1,nan", ";pk,100,1,0", "pk,,1,0"};
  for (const char *spec : bad) {
    if (dsp_spec_parse(spec, f, AIRPLAY_DSP_MAX_FILTERS, &n)) {
      fprintf(stderr, "accepted bad spec \"%s\"\n", spec);
      failures++;
    }
  }
  CHECK(n == 7 && near(f[0].frequency_hz, 1234));

  // Nine sections is one too many; eight worst-case sections fit HA's 255 characters.
  char nine[512] = "";
  for (int i = 0; i < 9; i++) strcat(nine, i ? ";pk,12500,10.00,-12.5" : "pk,12500,10.00,-12.5");
  CHECK(!dsp_spec_parse(nine, f, AIRPLAY_DSP_MAX_FILTERS, &n));
  nine[strlen(nine) - strlen(";pk,12500,10.00,-12.5")] = '\0';
  CHECK(dsp_spec_parse(nine, f, AIRPLAY_DSP_MAX_FILTERS, &n) && n == 8);
  len = dsp_spec_format(f, n, text, sizeof(text));
  CHECK(len > 0 && len <= 255);
  CHECK(!dsp_spec_parse("pk,100,1,0;pk,200,1,0", f, 1, &n));
  CHECK(dsp_spec_format(f, 8, text, 10) == 0);

  if (failures == 0) printf("ok\n");
  return failures == 0 ? 0 : 1;
}
