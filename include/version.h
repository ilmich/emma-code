#ifndef VERSION_H
#define VERSION_H

/**
 * @file version.h
 * @brief Single source of truth for the emma-code version string.
 *
 * The startup banner (see tui_show_banner() in include/tui.h) builds
 * its version line from this macro. Bump it manually on release.
 */
#define EMMA_VERSION "0.1.0"

#endif /* VERSION_H */
