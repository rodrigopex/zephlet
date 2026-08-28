#include "zlet_tick_interface.h"

/**
 * @file
 * @brief Consumer of a generated zephlet interface header.
 *
 * Referencing one generated enumerator is enough: this translation unit
 * cannot compile unless zlet_tick_interface.h -- and the nanopb header
 * that it includes in turn -- already exist when the compiler runs.
 */

const int consumer_tick_method_count = TICK_M__COUNT;
