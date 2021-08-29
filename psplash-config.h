/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2014 MenloSystems GmbH
 *  Author: Olaf Mandel <o.mandel@menlosystems.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef _HAVE_PSPLASH_CONFIG_H
#define _HAVE_PSPLASH_CONFIG_H

/* Text to output on program start; if undefined, output nothing */
#define PSPLASH_STARTUP_MSG "Setting up the system. This might take up to 15 minutes."

/* Bool indicating if the image is fullscreen, as opposed to split screen */
#ifndef PSPLASH_IMG_FULLSCREEN
#define PSPLASH_IMG_FULLSCREEN 0
#endif

/* Position of the image split from top edge, numerator of fraction */
#define PSPLASH_IMG_SPLIT_NUMERATOR 5

/* Position of the image split from top edge, denominator of fraction */
#define PSPLASH_IMG_SPLIT_DENOMINATOR 6

#endif
