/*
 *  Copyright (C) 2016, Xin ShuAn <shuan.xin@ingenic.com>
 *
 *  Ingenic QRcode SDK Project
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef GPIO_MANAGER_H
#define GPIO_MANAGER_H

#include <ingenic_api.h>
struct gpio_pin {
    uint32_t no;
    gpio_direction direction;
    gpio_irq_mode irq_mode;
    int fd;
    gpio_status valid;
};

#endif
