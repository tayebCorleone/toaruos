/*
 * Kernel Memory Manager
 * vim:tabstop=4
 * vim:noexpandtab
 */

#include <system.h>

extern uintptr_t end;
uintptr_t placement_pointer = (uintptr_t)&end;
uintptr_t heap_end = (uintptr_t)NULL;

void
kmalloc_startat(
		uintptr_t address
		) {
	placement_pointer = address;
}

/*
 * kmalloc() is the kernel's dumb placement allocator
 */
uintptr_t
kmalloc_real(
		size_t size,
		int align,
		uintptr_t * phys
		) {
	if (heap_end) {
		void * address;
		if (align) {
			address = valloc(size);
		} else {
			address = malloc(size);
		}
		if (phys) {
			page_t *page = get_page((uintptr_t)address, 0, kernel_directory);
			*phys = page->frame * 0x1000 + ((uintptr_t)address & 0xFFF);
		}
		return (uintptr_t)address;
	}

	if (align && (placement_pointer & 0xFFFFF000)) {
		placement_pointer &= 0xFFFFF000;
	}
	if (phys) {
		*phys = placement_pointer;
	}
	uintptr_t address = placement_pointer;
	placement_pointer += size;
	return (uintptr_t)address;
}
/*
 * Normal
 */
uintptr_t
kmalloc(
		size_t size
		) {
	return kmalloc_real(size, 0, NULL);
}
/*
 * Aligned
 */
uintptr_t
kvmalloc(
		size_t size
		) {
	return kmalloc_real(size, 1, NULL);
}
/*
 * With a physical address
 */
uintptr_t
kmalloc_p(
		size_t size,
		uintptr_t *phys
		) {
	return kmalloc_real(size, 0, phys);
}
/*
 * Aligned, with a physical address
 */
uintptr_t
kvmalloc_p(
		size_t size,
		uintptr_t *phys
		) {
	return kmalloc_real(size, 1, phys);
}

/*
 * Frame Allocation
 */

uint32_t *frames;
uint32_t nframes;

#define INDEX_FROM_BIT(b) (b / 0x20)
#define OFFSET_FROM_BIT(b) (b % 0x20)

static void
set_frame(
		uintptr_t frame_addr
		) {
	uint32_t frame  = frame_addr / 0x1000;
	uint32_t index  = INDEX_FROM_BIT(frame);
	uint32_t offset = OFFSET_FROM_BIT(frame);
	frames[index] |= (0x1 << offset);
}

static void
clear_frame(
		uintptr_t frame_addr
		) {
	uint32_t frame  = frame_addr / 0x1000;
	uint32_t index  = INDEX_FROM_BIT(frame);
	uint32_t offset = OFFSET_FROM_BIT(frame);
	frames[index] &= ~(0x1 << offset);
}

static uint32_t
test_frame(
		uintptr_t frame_addr
		) {
	uint32_t frame  = frame_addr / 0x1000;
	uint32_t index  = INDEX_FROM_BIT(frame);
	uint32_t offset = OFFSET_FROM_BIT(frame);
	return (frames[index] & (0x1 << offset));
}

static uint32_t
first_frame() {
	uint32_t i, j;
	for (i = 0; i < INDEX_FROM_BIT(nframes); ++i) {
		if (frames[i] != 0xFFFFFFFF) {
			for (j = 0; j < 32; ++j) {
				uint32_t test_frame = 0x1 << j;
				if (!(frames[i] & test_frame)) {
					return i * 0x20 + j;
				}
			}
		}
	}
	return -1;
}

void
alloc_frame(
		page_t *page,
		int is_kernel,
		int is_writeable
		) {
	if (page->frame) {
		page->rw      = (is_writeable == 1) ? 1 : 0;
		page->user    = (is_kernel == 1)    ? 0 : 1;
		return;
	} else {
		uint32_t index = first_frame();
		if (index == (uint32_t)-1) {
			HALT_AND_CATCH_FIRE("Failed to allocate a frame: out of frames");
		}
		set_frame(index * 0x1000);
		page->present = 1;
		page->rw      = (is_writeable == 1) ? 1 : 0;
		page->user    = (is_kernel == 1)    ? 0 : 1;
		page->frame   = index;
	}
}

void
free_frame(
		page_t *page
		) {
	uint32_t frame;
	if (!(frame = page->frame)) {
		return;
	} else {
		clear_frame(frame);
		page->frame = 0x0;
	}
}

void
paging_install(uint32_t memsize) {
	nframes = memsize  / 4;
	frames  = (uint32_t *)kmalloc(INDEX_FROM_BIT(nframes));
	memset(frames, 0, INDEX_FROM_BIT(nframes));

	uintptr_t phys;
	kernel_directory = (page_directory_t *)kvmalloc_p(sizeof(page_directory_t),&phys);
	memset(kernel_directory, 0, sizeof(page_directory_t));

	uint32_t i = 0;
	while (i < placement_pointer + 0x1000) {
		alloc_frame(get_page(i, 1, kernel_directory), 0, 0);
		i += 0x1000;
	}
	isrs_install_handler(14, page_fault);
	kernel_directory->physical_address = (uintptr_t)kernel_directory->physical_tables;


	current_directory = clone_directory(kernel_directory);
	switch_page_directory(kernel_directory);
}

void
switch_page_directory(
		page_directory_t * dir
		) {
	current_directory = dir;
	__asm__ __volatile__ ("mov %0, %%cr3":: "r"(dir->physical_address));
	uint32_t cr0;
	__asm__ __volatile__ ("mov %%cr0, %0": "=r"(cr0));
	cr0 |= 0x80000000;
	__asm__ __volatile__ ("mov %0, %%cr0":: "r"(cr0));
}

page_t *
get_page(
		uintptr_t address,
		int make,
		page_directory_t * dir
		) {
	address /= 0x1000;
	uint32_t table_index = address / 1024;
	if (dir->tables[table_index]) {
		return &dir->tables[table_index]->pages[address % 1024];
	} else if(make) {
		uint32_t temp;
		dir->tables[table_index] = (page_table_t *)kvmalloc_p(sizeof(page_table_t), (uintptr_t *)(&temp));
		memset(dir->tables[table_index], 0, 0x1000);
		dir->physical_tables[table_index] = temp | 0x7; /* Present, R/w, User */
		return &dir->tables[table_index]->pages[address % 1024];
	} else {
		return 0;
	}
}

void
page_fault(
		struct regs *r)  {
	uint32_t faulting_address;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(faulting_address));

	int present  = !(r->err_code & 0x1);
	int rw       = r->err_code & 0x2;
	int user     = r->err_code & 0x4;
	int reserved = r->err_code & 0x8;
	int id       = r->err_code & 0x10;

	kprintf("Page fault! (p:%d,rw:%d,user:%d,res:%d,id:%d) at 0x%x\n", present, rw, user, reserved, id, faulting_address);
	HALT_AND_CATCH_FIRE("Page fault");
}

/*
 * Heap
 * Stop using kalloc and friends after installing the heap
 * otherwise shit will break. I've conveniently broken
 * kalloc when installing the heap, just for those of you
 * who feel the need to screw up.
 */


void
heap_install() {
	heap_end = (placement_pointer + 0x1000) & ~0xFFF;
	placement_pointer = 0;
}

void *
sbrk(
	uintptr_t increment
    ) {
	ASSERT(increment % 0x1000 == 0);
	ASSERT(heap_end % 0x1000 == 0);
	uintptr_t address = heap_end;
	heap_end += increment;
	uintptr_t i;
	for (i = address; i < heap_end; i += 0x1000) {
		get_page(i, 1, kernel_directory);
		alloc_frame(get_page(i, 1, kernel_directory), 0, 1);
	}
	return (void *)address;
}
