// The runner includes console_locale directly from misc.cpp; these expectations pin the Wii U system numbers.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "console_locale.inc"

int main() {
    unsetenv("BOTW_REGION"); unsetenv("BOTW_LANG");
    auto value = console_locale();
    assert(value.region == 2 && value.country == 49 && value.language == 1);
    setenv("BOTW_LANG", "ja", 1);
    value = console_locale();
    assert(value.region == 2 && value.country == 49 && value.language == 1);
    struct Case { const char* region; const char* language; uint32_t r, c, l; };
    const Case cases[] = {
        {"JP", "ja", 1, 1, 0}, {"US", "en", 2, 49, 1}, {"US", "fr", 2, 49, 2}, {"US", "es", 2, 49, 5},
        {"EU", "en", 4, 110, 1}, {"EU", "de", 4, 78, 3}, {"EU", "fr", 4, 77, 2}, {"EU", "es", 4, 105, 5},
        {"EU", "it", 4, 83, 4}, {"EU", "nl", 4, 94, 8}, {"EU", "ru", 4, 100, 10}, {"bad", "ja", 2, 49, 1},
        {"JP", "en", 1, 1, 0}, {"US", "de", 2, 49, 1}, {"EU", "bad", 4, 110, 1},
    };
    for (const auto& test : cases) {
        setenv("BOTW_REGION", test.region, 1); setenv("BOTW_LANG", test.language, 1);
        value = console_locale();
        assert(value.region == test.r && value.country == test.c && value.language == test.l);
    }
    std::cout << "PASS: runtime locale defaults and 15 region/language mappings\n";
}
