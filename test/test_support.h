#pragma once

#include <stddef.h>

namespace test_support {

void resetRuntime();
size_t createdTaskCount();
size_t deletedTaskCount();

}  // namespace test_support
