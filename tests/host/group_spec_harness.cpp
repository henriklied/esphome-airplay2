// Host harness for tests/test_group_spec.py: audio/group_spec.cpp as it builds for the board.
#include <cstdio>
#include <cstring>

#include "audio/group_spec.h"

using namespace esphome::airplay_receiver;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      failures++;                                                       \
    }                                                                   \
  } while (0)

int main() {
  GroupSpec g;
  char text[256];

  // Today's Kjøkkenet pair, both ends.
  CHECK(group_spec_parse("role=primary;peer=10.0.40.146;channel=stereo;name=Kjøkkenet", &g));
  CHECK(g.role == PeerRole::PRIMARY && strcmp(g.peer, "10.0.40.146") == 0);
  CHECK(g.channel == PeerForward::STEREO && strcmp(g.name, "Kjøkkenet") == 0 && g.port == 7011);
  CHECK(group_spec_format(g, text, sizeof(text)) > 0);
  CHECK(strcmp(text, "role=primary;peer=10.0.40.146;channel=stereo;name=Kjøkkenet") == 0);

  CHECK(group_spec_parse("role=secondary;peer=10.0.40.90;clock=relay;delay=250;standalone=1", &g));
  CHECK(g.role == PeerRole::SECONDARY && g.clock == PeerClockMode::RELAY && g.delay_us == 250 && g.standalone);
  CHECK(group_spec_format(g, text, sizeof(text)) > 0);
  CHECK(strcmp(text, "role=secondary;peer=10.0.40.90;clock=relay;delay=250;standalone=1") == 0);
  GroupSpec back;
  CHECK(group_spec_parse(text, &back) && back.delay_us == 250 && back.standalone);

  // Defaults, and a secondary that accepts any primary.
  CHECK(group_spec_parse("role=secondary", &g) && g.peer[0] == '\0' && g.channel == PeerForward::RIGHT &&
        !g.standalone && g.port == 7011);
  CHECK(group_spec_parse("role=primary;peer=10.0.40.146;port=7020", &g) && g.port == 7020 && g.name[0] == '\0');
  CHECK(group_spec_parse("standalone", &g) && g.role == PeerRole::STANDALONE);
  CHECK(group_spec_format(g, text, sizeof(text)) == 10 && strcmp(text, "standalone") == 0);

  // Rejected without touching the output.
  g.delay_us = 1234;
  const char *bad[] = {"", "role=primary", "role=boss", "peer=10.0.40.1", "role=secondary;peer=10.0.40",
                       "role=secondary;peer=10.0.40.256", "role=secondary;peer=1.2.3.4.5", "role=secondary;peer=a.b.c.d",
                       "role=secondary;delay=2000000", "role=secondary;delay=-1", "role=secondary;delay=100001", "role=secondary;delay=12ms", "role=primary;peer=1.2.3.4;channel=up",
                       "role=secondary;standalone=yes", "role=secondary;port=0", "role=secondary;colour=red",
                       "role=secondary;oops",
                       "role=primary;peer=1.2.3.4;name=0123456789012345678901234567890123456789012345678901234567890123"};
  for (const char *spec : bad) {
    if (group_spec_parse(spec, &g)) {
      fprintf(stderr, "accepted bad spec \"%s\"\n", spec);
      failures++;
    }
  }
  CHECK(g.delay_us == 1234);
  CHECK(group_spec_format(g, text, 8) == 0);

  if (failures == 0) printf("ok\n");
  return failures == 0 ? 0 : 1;
}
