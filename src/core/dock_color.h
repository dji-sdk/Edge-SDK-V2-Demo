/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * dock_color.h -- terminal coloring of command results (internal use). OK green, failure red.
 */
#ifndef DOCK_COLOR_H
#define DOCK_COLOR_H

#define DOCK_GRN  "\033[32m"
#define DOCK_RED  "\033[31m"
#define DOCK_RST  "\033[0m"

#define DOCK_OK    DOCK_GRN "OK" DOCK_RST      /* success marker (green) */
#define DOCK_FAIL  DOCK_RED "FAIL" DOCK_RST    /* failure marker (red) */

#endif /* DOCK_COLOR_H */
