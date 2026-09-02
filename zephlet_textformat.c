#include "zephlet.pb.h"

#include "zephlet_textformat.h"

/**
 * @file
 * @brief Text-format descriptors for the shared zephlet.proto messages.
 *
 * One translation unit, compiled whenever CONFIG_NANOPB_TEXTFORMAT is on,
 * so the descriptor exists for anything that wants to print a
 * Lifecycle.Status -- a frontend, or application logging. See the header
 * for why codegen cannot emit this one.
 */

PB_TF_DEFINE(LIFECYCLE_STATUS, lifecycle_status_t);
