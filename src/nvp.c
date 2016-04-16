#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "nvp.h"
#include "nvsim.h"

static struct rb_root nvpcache_root = RB_ROOT;

void *nvpcache_search(int _nvid) {
    struct nvpitem *item = nvpcache_search_rb(&nvpcache_root, _nvid);
    if (!item) return NULL;
    return item->vaddr;
}

struct nvpitem *nvpcache_search_foritem(int _nvid) {
   return nvpcache_search_rb(&nvpcache_root, _nvid);
}

static struct nvpitem *nvpcache_search_rb(struct rb_root *root, int _nvid) {
    struct rb_node *node = root->rb_node;
    while (node) {
        struct nvpitem *data = container_of(node, struct nvpitem, node);
        struct nvp_t *nvp_ptr = &data->nvp;
        if (_nvid < nvp_ptr->nvid)
            node = node->rb_left;
        else if (_nvid > nvp_ptr->nvid)
            node = node->rb_right;
        else
            return data;
    }
    return NULL;
}

int nvpcache_insert(int _nvid, int offset, int size, void *addr) {
	struct nvpitem *_nvpitem = malloc(sizeof(struct nvpitem));
	struct nvp_t *nvp_ptr = &_nvpitem->nvp;
	nvp_ptr->nvid = _nvid;
	nvp_ptr->nvoffset = offset;
	nvp_ptr->size = size;
	_nvpitem->vaddr = addr;
    return nvpcache_insert_rb(&nvpcache_root, _nvpitem);
}

static int nvpcache_insert_rb(struct rb_root *root, struct nvpitem *_nvpitem) {
    struct rb_node **new = &(root->rb_node), *parent = NULL;
    int _nvid = _nvpitem->nvp.nvid;
//    printf("%s nvid %d\n", __func__, _nvid);
    while (*new) {
        struct nvpitem *this = container_of(*new, struct nvpitem, node);
        parent = *new;
        struct nvp_t *nvp_ptr = &this->nvp;
        if (_nvid < nvp_ptr->nvid)
            new = &((*new)->rb_left);
        else if (_nvid > nvp_ptr->nvid)
            new = &((*new)->rb_right);
        else
            return -1;
    }
    rb_link_node(&_nvpitem->node, parent, new);
    rb_insert_color(&_nvpitem->node, root);
    return 0;
}

int nvpcache_delete(int _nvid) {
    return nvpcache_delete_rb(&nvpcache_root, _nvid);
}

static int nvpcache_delete_rb(struct rb_root *root, int _nvid) {
    struct nvpitem *item = nvpcache_search_rb(root, _nvid);
    if (!item) return -1;
    rb_erase(&item->node, root);
    free(item);
    return 0;
}

struct nvp_t alloc_nvp(int _nvid, int size) {
	// TODO check nvid not exist
	void *vaddr = nv_get(_nvid, size);
	nvpcache_insert(_nvid, 0, size, vaddr);
	struct nvp_t nvp_ret;
	nvp_ret.nvid = _nvid;
	nvp_ret.nvoffset = 0;
	nvp_ret.size = size;
	return nvp_ret;
}

void *get_nvp(struct nvp_t *nvp) {
	if (nvp == NULL) {
		printf("%s: NULL argument\n", __func__);
		return NULL;
	}
	// TODO check nvid exist in shm
	void *vaddr = nvpcache_search(nvp->nvid);
	if (vaddr != NULL) {
		return vaddr;
	}
	vaddr = nv_attach(nvp->nvid);
	nvpcache_insert(nvp->nvid, nvp->nvoffset, nvp->size, vaddr);
	return vaddr;
}

void free_nvp(struct nvp_t *nvp) {
	// TODO check nvid exist in shm
//	printf("%s nvid %d\n", __func__, nvp->nvid);
	void *vaddr = nvpcache_search(nvp->nvid);
	if (vaddr != NULL) {
		nvpcache_delete(nvp->nvid);
	}
	nv_remove(nvp->nvid);
}

/*
 * nv_allocator
 */

static void *heap_base_addr = 0;

#define HEAP_SIZE_MIN 131072 // 128kB
#define HEAP_CHUNK_SIZE 128 // 128B per chunk
#define HEAP_MAGIC 0x5A1AA1A5

/*
 * memory image:
 * HEAP_MAGIC | h_nvid | size | BITMAP | data
 */
/*
 * helper function
 */
static int get_heap_size() {
	assert(heap_base_addr != 0);
	int *hsize_ptr = heap_base_addr + 2 * sizeof(int);
	return *hsize_ptr;
}

static int get_bitmap_bits() {
	assert(heap_base_addr != 0);
	int heap_size = get_heap_size();
	return ((heap_size - 3 * sizeof(int)) * 8) / (HEAP_CHUNK_SIZE * 8 + 1);
}

static void *get_bitmap_addr() {
	assert(heap_base_addr != 0);
	return heap_base_addr + 3 * sizeof(int);
}

static void *get_data_addr() {
	assert(heap_base_addr != 0);
	return heap_base_addr + 3 * sizeof(int) + get_bitmap_bits() / 8;
}

static int get_heap_nvid() {
	assert(heap_base_addr != 0);
	int *hvid_ptr = heap_base_addr + sizeof(int);
	return *hvid_ptr;
}

void nvalloc_init(int h_nvid, int heap_size) {
	if (nv_exist(h_nvid) != -1) {
		// nvpcache
		heap_base_addr = nvpcache_search(h_nvid);
		if (heap_base_addr == NULL) {
			heap_base_addr = nv_attach(h_nvid);
			nvpcache_insert(h_nvid, 0, get_heap_size(), heap_base_addr);
		}
		int *magic_ptr = (int *)heap_base_addr;
		if (*magic_ptr != HEAP_MAGIC) {
			printf("HEAP_MAGIC error!\n");
			exit(EXIT_FAILURE);
		}
//		printf("heap_base_addr: %p\n", heap_base_addr);
		return;
	}
	if (heap_size < HEAP_SIZE_MIN) {
		heap_size = HEAP_SIZE_MIN;
	}
	heap_base_addr = nv_get(h_nvid, heap_size);
	// nvpcache
	nvpcache_insert(h_nvid, 0, heap_size, heap_base_addr);

//	printf("heap_base_addr: %p\n", heap_base_addr);
	int *go_ptr = (int *)heap_base_addr;
	*go_ptr = HEAP_MAGIC;
	++go_ptr;
	*go_ptr = h_nvid;
	++go_ptr;
	*go_ptr = heap_size;
	++go_ptr;
	// set bitmap to 0
	memset((void *)go_ptr, 0, get_bitmap_bits() / 8);
	return;
}

struct nvp_t nvalloc_malloc(int size) {
	assert(heap_base_addr != 0);

	// first fit?
	// ceil the chunk_num
	double test = (double)size / HEAP_CHUNK_SIZE;
	int chunk_num = (int)test;
	if ((test-chunk_num) > 0) {
		++chunk_num;
	}
	int total_chunk_num = get_bitmap_bits();
	char *bitmap_addr = get_bitmap_addr();
//	printf("heap_base_addr %p, bitmap_addr %p\n", heap_base_addr, bitmap_addr);
	int res_index = -1;
	int i, j;
//	printf("total_chunk_num %d, chunk_num %d\n", total_chunk_num, chunk_num);
	for (i = 0; i <= (total_chunk_num - chunk_num); ++i) {
		for (j = 0; j < chunk_num; ++j) {
			int index = i + j;
			int seg = index / 8;
			int offset = index % 8;
			int state = bitmap_addr[seg] & (0x1 << (7-offset));
			if (state != 0) {
				break;
			} else {
				if (j == (chunk_num-1)) {
					// find
					res_index = i;
				}
			}
		}
		if (res_index != -1) {
			break;
		}
	}

	if (res_index == -1) {
		// not find
		printf("heap memory not enough\n");
		exit(EXIT_FAILURE);
	}
//	printf("res_index %d, chunk_num %d\n", res_index, chunk_num);
	// set bitmap to 1
	for (j=0; j<chunk_num; ++j) {
		int index = res_index + j;
		int seg = index / 8;
		int offset = index % 8;
		bitmap_addr[seg] |= (0x1 << (7-offset));
//		printf("seg %d, offset %d, bitmap %x\n", seg, offset, (unsigned char)bitmap_addr[seg]);
	}
	// return nvp
	struct nvp_t nvp;
	nvp.nvid = get_heap_nvid();
	nvp.nvoffset = res_index * HEAP_CHUNK_SIZE;
	nvp.size = size;
	return nvp;
}

inline void *nvalloc_getnvp(struct nvp_t *nvp) {
	assert(heap_base_addr != 0);
	if (nvp == NULL) {
		printf("%s: NULL argument\n", __func__);
		return NULL;
	}
	return get_data_addr() + nvp->nvoffset;
}

void nvalloc_free(struct nvp_t *nvp) {
	assert(heap_base_addr != 0);

	int res_index = nvp->nvoffset / HEAP_CHUNK_SIZE;
	double test = (double) nvp->size / HEAP_CHUNK_SIZE;
	int chunk_num = (int) test;
	if ((test - chunk_num) > 0) {
		++chunk_num;
	}
	char *bitmap_addr = get_bitmap_addr();
	// set bitmap to 0
	int j;
	for (j = 0; j < chunk_num; ++j) {
		int index = res_index + j;
		int seg = index / 8;
		int offset = index % 8;
		bitmap_addr[seg] &= ~(0x1 << (7 - offset));
	}
}

struct nvp_t make_nvp_withdata(void *d, int dsize) {
	assert(heap_base_addr != 0);

	struct nvp_t d_nvp = nvalloc_malloc(dsize);
	void *d_nv = nvalloc_getnvp(&d_nvp);
	memcpy(d_nv, d, dsize);
	return d_nvp;
}