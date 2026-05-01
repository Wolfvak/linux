#ifndef __MACH_SLOT1_H
#define __MACH_SLOT1_H

#include <linux/types.h>
#include <linux/of_platform.h>

enum ntr_slot1_mode {
    NTR_SLOT1_DISABLED,
    NTR_SLOT1_SPI,
    NTR_SLOT1_ROM
};

struct platform_device *ntr_slot1_find_pdev(void);

void __iomem *ntr_slot1_lock(struct platform_device *pdev, enum ntr_slot1_mode mode);
int ntr_slot1_release(struct platform_device *pdev);

#endif
