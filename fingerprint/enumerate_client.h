/*
 *  Copyright (C) 2017, Zhang YanMing <yanmin.zhang@ingenic.com, jamincheung@126.com>
 *
 *  Ingenic Linux plarform SDK project
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

#ifndef ENUMERATE_CLIENT_H
#define ENUMERATE_CLIENT_H

#include "client_monitor.h"

struct enumerate_client {
    struct client_monitor* base;

    void (*construct)(struct enumerate_client* this, int64_t device_id,
            struct fingerprint_client_sender* sender, int user_id, int group_id);
    void (*destruct)(struct enumerate_client* this);
};

void construct_enumerate_client(struct enumerate_client* this,
        int64_t device_id, struct fingerprint_client_sender* sender,
        int user_id, int group_id);
void destruct_enumerate_client(struct enumerate_client* this);

#endif /* ENUMERATE_CLIENT_H */