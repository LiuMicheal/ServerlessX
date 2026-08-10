#include "kernel_helper.h"

#define DEFAULT_PERMISSION S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH

long mac_id = 0;
module_param(mac_id, long, DEFAULT_PERMISSION);

long gid_index = 0;
module_param(gid_index, long, DEFAULT_PERMISSION);

unsigned long peer_mac = 0;
module_param(peer_mac, ulong, DEFAULT_PERMISSION);
