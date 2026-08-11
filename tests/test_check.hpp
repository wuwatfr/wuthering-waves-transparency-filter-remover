#pragma once

#include <cstdio>
#include <cstdlib>

#define CHECK(expression)                                                        \
  do {                                                                           \
    if (!(expression)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                   #expression);                                                \
      std::fflush(stderr);                                                       \
      std::exit(EXIT_FAILURE);                                                   \
    }                                                                            \
  } while (false)
