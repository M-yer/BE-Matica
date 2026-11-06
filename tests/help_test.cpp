#include "help_text.hpp"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
  using help::Lang;
  const size_t count = help::topics(Lang::English).size();
  for (Lang l : {Lang::English, Lang::Spanish, Lang::Portuguese}) {
    CHECK(help::topics(l).size() == count);
    for (const auto &t : help::topics(l)) {
      CHECK(std::strlen(t.title) > 0);
      CHECK(std::strlen(t.text) > 0);
      CHECK(std::strlen(t.text) < 1024);  // UTF-8 bytes, which is what the launcher counts
    }
  }
  CHECK(help::langForLocale("es") == Lang::Spanish);
  CHECK(help::langForLocale("es_MX") == Lang::Spanish);
  CHECK(help::langForLocale("es-419") == Lang::Spanish);
  CHECK(help::langForLocale("pt-BR") == Lang::Portuguese);
  CHECK(help::langForLocale("pt_PT") == Lang::Portuguese);
  CHECK(help::langForLocale("en-US") == Lang::English);
  CHECK(help::langForLocale("fr") == Lang::English);
  CHECK(help::langForLocale("") == Lang::English);
  if (!failures) std::puts("help_test OK");
  return failures ? 1 : 0;
}
