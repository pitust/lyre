#ifndef _MM__PMM_H
#define _MM__PMM_H

#include <stddef.h>
#include <limine.h>

extern volatile struct limine_memmap_request memmap_request;

void pmm_init(void);
void *pmm_alloc(size_t pages);
void *pmm_alloc_nozero(size_t pages);
void pmm_free(void *addr, size_t pages);

uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_reserved_pages(void);

#endif
