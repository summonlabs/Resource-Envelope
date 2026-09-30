#include <iostream>

#include "support/testing.hpp"

int main() {
  int status = ::testing::run_all();
  std::cout.flush();
  return status;
}
