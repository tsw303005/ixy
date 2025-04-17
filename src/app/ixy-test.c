#include <stdio.h>

#include "driver/device.h"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <pci bus id>\n", argv[0]);
        return 1;
    }


    struct ixy_device* dev = ixy_init(argv[1], 1, 1, 0);
}
