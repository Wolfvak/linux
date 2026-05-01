#ifndef __MACH_TCM_H
#define __MACH_TCM_H

#include <linux/types.h>

void *ntr_alloc_itcm(size_t len);
void ntr_free_itcm(void *ptr, size_t len);

void *ntr_alloc_dtcm(size_t len);
void ntr_free_dtcm(void *ptr, size_t len);

#endif
