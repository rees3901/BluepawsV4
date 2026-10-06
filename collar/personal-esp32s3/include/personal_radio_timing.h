#pragma once
#include <bp_config.h>

// Personal adapters use the V4 30-second receive policy without editing the
// canonical protocol copy on this isolated branch. ACK retry timing is unchanged.
#undef CMD_LISTEN_WINDOW_MS
#define CMD_LISTEN_WINDOW_MS 30000
