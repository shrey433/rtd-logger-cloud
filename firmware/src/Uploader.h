#pragma once
#include <stddef.h>

#include "ReadingQueue.h"

namespace Uploader {
enum class Result {
  Ok,     // server stored the rows (2xx); safe to forget them
  Drop,   // server says the payload itself is bad (400/413/422); resending can never succeed
  Retry,  // network error, timeout, 5xx, or an auth problem; keep the rows and try again
};

// POSTs the rows (oldest first) to SERVER_URL in the JSON shape the cloud server expects.
Result post(const Reading* rows, size_t count);
}  // namespace Uploader
