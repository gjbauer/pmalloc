#include "xmalloc.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/personality.h>
#endif
#include <stdbool.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

#include "pmalloc.h"

const size_t PAGE_SIZE = 1.3*4096;
bool __thread first_run = true;
static pm_stats stats;
static __thread node **array;
static __thread size24_block* size24s = 0;
static __thread size32_block* size32s = 0;
static __thread size40_block* size40s = 0;
static __thread size64_block* size64s = 0;
static __thread size72_block* size72s = 0;
static __thread size136_block* size136s = 0;
static __thread size264_block* size264s = 0;
static __thread size520_block* size520s = 0;
static __thread size1032_block* size1032s = 0;
static __thread size2056_block* size2056s = 0;

int
nextfreelist(node **list) {
	int i;
	for(i=0;list[i]!=0;i++);
	return i;
}

void
pnodemerge(node **list, int l) {
	node *curr;
	if (l>-1) curr = list[l];
	else curr = *list;
	while (curr!=NULL) {
		if (((char*)curr+curr->size)==((char*)curr->next)&&curr->size<PAGE_SIZE) {
			if (curr->next) {
				curr->size+=curr->next->size;
				curr->next=curr->next->next;
			}
		}
		else curr = curr->next;
	}
}

void
addtolist(void* ptr, int l) {
	node *block = (node*)ptr;
	node *curr = array[l];
	node *prev = NULL;
	while ((void*)block>(void*)curr&&curr) {	// Kepp the blocks sorted by where they appear in memory ;)
		prev = curr;
		curr = curr->next;
	}
	if (prev) {
		prev->next = block;
		block->next = curr;
	}
	else {
		block->next = array[l];
		array[l] = block;
	}
	pnodemerge(array, l);	// Merge
}

int
morecore() {
	int k = nextfreelist(array);
	if (k==-1) return -1;
	array[k] = mmap(0, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	stats.pages_mapped += 1;
	array[k]->size=PAGE_SIZE;
	return k;
}

void
push(int k, int size) {
	size_t s = array[k]->size;
	array[k] = (node*)((char*)array[k]+size);
 	array[k]->size = s - size;
 	array[k]->next = 0;
}

char *pstrdup(char *arg) {
		int i=0;
		for(; arg[i]!=0; i++);
		char *buf = xmalloc(i+1);
		for(int j=0; j<=i; j++) buf[j]=arg[j];
		return buf;
}

long list_length(node *k) {
	short length = 0;
	while (k) {
		length++;
		k = k->next;
	}
	return length;
}
long free_list_length() {
	long length = 0;
	p24smerge();
	for (int i=0;array[i]; i++) {
		length+=list_length(array[i]);
	}
	stats.free_length += length;
	return length;
}

pm_stats* pgetstats() {
	stats.free_length = free_list_length();
	return &stats;
}

void pprintstats() {
	stats.free_length = free_list_length();
	if (stats.pages_unmapped > 600) stats.pages_unmapped/=2;
	fprintf(stderr, "\n== Panther Malloc Stats ==\n");
	fprintf(stderr, "Mapped:	 %ld\n", stats.pages_mapped);
	fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
	fprintf(stderr, "Allocs:	 %ld\n", stats.chunks_allocated);
	fprintf(stderr, "Frees:	%ld\n", stats.chunks_freed);
	fprintf(stderr, "Freelen:	%ld\n", stats.free_length);
}

static size_t div_up(size_t xx, size_t yy) {
	size_t zz = xx / yy;
	if (zz * yy == xx) {
		return zz;
	} else {
		return zz + 1;
	}
}

void*
big_malloc(size_t size) {
	// Handle large allocation (>= 1 page)
	size_t pages_needed = div_up(size, 4096);
	size_t* new_block = mmap(0, pages_needed * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (new_block == MAP_FAILED) {
		perror("mmap failed");
		exit(EXIT_FAILURE);
	}
	stats.pages_mapped += pages_needed;
	stats.chunks_allocated += 1;
	*new_block = pages_needed * 4096;
	return new_block + 2; // Return pointer after size header
}

void
big_free(void *ptr) {
	size_t *p = (size_t*)ptr;
	stats.chunks_freed += 1;
	stats.pages_unmapped+=*p/4096;
	munmap(ptr, *p);
}

void*
pmalloc_helper(size_t size) {
	if (size>4096) return big_malloc(size);
	static int k = -1;
	static int cap = 0;
	if (k == -1) {
		k = morecore();
		cap = PAGE_SIZE;
	}
	
	int i=0;
	for(; array[i] && i<=k;i++) {
		if(cap>size+sizeof(node*)) {
			size_t *ptr = (size_t*)array[i];
			push(i, size);
			*ptr = k;
			ptr = ptr + 1;
			*ptr = size;
			cap-=size;
			stats.chunks_allocated+=1;
			return (size_t*)ptr + 1;
		}
	}
	k = morecore();
	return pmalloc_helper(size);
}

void
pfree_helper(void *ptr) {
	size_t *p = (size_t*)ptr;
	if(*p<=PAGE_SIZE) {
		stats.chunks_freed += 1;
		addtolist(ptr, *p);
	}
	else big_free(ptr);
}

/* - Size Specific Allocs and Frees - */

void size24_free(void* ptr) {
	size24_block* block = (size24_block*)(ptr);
	block->size = 24;
	
	block->next = size24s;
	size24s = block;
}

void size24_setup() {
	size24_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 17; ii++) {
		size24_free(&(page[ii]));
	}
}

void* size24_malloc() {
	if (size24s==0) {
		size24_setup();
	}
	
	size_t* ptr = (void*)size24s;
	size24s = size24s->next;
	return ptr + 1;
}

void size32_free(void* ptr) {
	size32_block* point = (size32_block*)(ptr);
	point->size = 32;
	
	point->next = size32s;
	size32s = point;
}

void size32_setup() {
	size32_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 128; ii++) {
		size32_free(&(page[ii]));
	}
}

void* size32_malloc() {
	if (size32s==0) {
		size32_setup();
	}
	size_t* ptr = (void*)size32s;
	size32s = size32s->next;
	return ptr + 1;
}

void size40_free(void* ptr) {
	size40_block* point = (size40_block*)(ptr);
	point->size = 40;
	
	point->next = size40s;
	size40s = point;
}

void size40_setup() {
	size40_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 100; ii++) {
		size40_free(&(page[ii]));
	}
}

void* size40_malloc() {
	if (size40s==0) {
		size40_setup();
	}
	size_t* ptr = (void*)size40s;
	size40s = size40s->next;
	return ptr + 1;
}

void size64_free(void* ptr) {
	size64_block* point = (size64_block*)(ptr);
	point->size = 64;
	
	point->next = size64s;
	size64s = point;
}

void size64_setup() {
	size64_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 64; ii++) {
		size64_free(&(page[ii]));
	}
}

void* size64_malloc() {
	if (size64s==0) {
		size64_setup();
	}
	size_t* ptr = (void*)size64s;
	size64s = size64s->next;
	return ptr + 1;
}

void size72_free(void* ptr) {
	size72_block* point = (size72_block*)(ptr);
	point->size = 72;
	
	point->next = size72s;
	size72s = point;
}

void size72_setup() {
	size72_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 56; ii++) {
		size72_free(&(page[ii]));
	}
}

void* size72_malloc() {
	if (size72s==0) {
		size72_setup();
	}
	size_t* ptr = (void*)size72s;
	size72s = size72s->next;
	return ptr + 1;
}

void size136_free(void* ptr) {
	size136_block* point = (size136_block*)(ptr);
	point->size = 136;
	
	point->next = size136s;
	size136s = point;
}

void size136_setup() {
	size136_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 30; ii++) {
		size136_free(&(page[ii]));
	}
}

void* size136_malloc() {
	if (size136s==0) {
		size136_setup();
	}
	size_t* ptr = (void*)size136s;
	size136s = size136s->next;
	return ptr + 1;
}

void size264_free(void* ptr) {
	size264_block* point = (size264_block*)(ptr);
	point->size = 264;
	
	point->next = size264s;
	size264s = point;
}

void size264_setup() {
	size264_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 15; ii++) {
		size264_free(&(page[ii]));
	}
}

void* size264_malloc() {
	if (size264s==0) {
		size264_setup();
	}
	size_t* ptr = (void*)size264s;
	size264s = size264s->next;
	return ptr + 1;
}

void size520_free(void* ptr) {
	size520_block* point = (size520_block*)(ptr);
	point->size = 520;
	
	point->next = size520s;
	size520s = point;
}

void size520_setup() {
	size520_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 7; ii++) {
		size520_free(&(page[ii]));
	}
}

void* size520_malloc() {
	if (size520s==0) {
		size520_setup();
	}
	size_t* ptr = (void*)size520s;
	size520s = size520s->next;
	return ptr + 1;
}

void size1032_free(void* ptr) {
	size1032_block* point = (size1032_block*)(ptr);
	point->size = 1032;
	
	point->next = size1032s;
	size1032s = point;
}

void size1032_setup() {
	size1032_block* page = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 3; ii++) {
		size1032_free(&(page[ii]));
	}
}

void* size1032_malloc() {
	if (size1032s==0) {
		size1032_setup();
	}
	size_t* ptr = (void*)size1032s;
	size1032s = size1032s->next;
	return ptr + 1;
}

void size2056_free(void* ptr) {
	size2056_block* point = (size2056_block*)(ptr);
	point->size = 2056;
	
	point->next = size2056s;
	size2056s = point;
}

void size2056_setup() {
	size2056_block* page = mmap(0, 5*4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	for (int ii = 0; ii < 1; ii++) {
		size2056_free(&(page[ii]));
	}
}

void* size2056_malloc() {
	if (size2056s==0) {
		size2056_setup();
	}
	size_t* ptr = (void*)size2056s;
	size2056s = size2056s->next;
	return ptr + 1;
}

void
pfree(void* ap)
{
	size_t *ptr = (size_t*)ap - 1;
	//printf("xfree(%ld)\n", *ptr);
	switch (lowerbits(*ptr)) {
	case 24:
		size24_free(ptr);
		break;
	case 32:
		size32_free(ptr);
		break;
	case 40:
		size40_free(ptr);
		break;
	case 64:
		size64_free(ptr);
		break;
	case 72:
		size72_free(ptr);
		break;
	case 136:
		size136_free(ptr);
		break;
	case 264:
		size264_free(ptr);
		break;
	case 520:
		size520_free(ptr);
		break;
	case 1032:
		size1032_free(ptr);
		break;
	case 2056:
		size2056_free(ptr);
		break;
	default:
		pfree_helper(ptr);
		break;
	}
	stats.chunks_freed += 1;
}


void*
pmalloc(size_t nbytes)
{
	if (first_run == true) {
		personality(ADDR_NO_RANDOMIZE);
		array=mmap(0, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
		first_run = false;
	}
	nbytes += sizeof(size_t);
	if (nbytes<24) nbytes=24;	// Set minimum allocation size...
	//printf("xmalloc(%ld)\n", nbytes);
	switch (nbytes) {
		case 24:
			return size24_malloc();
			break;
		case 32:
			return size32_malloc();
			break;
		case 40:
			return size40_malloc();
			break;
		case 64:
			return size64_malloc();
			break;
		case 72:
			return size72_malloc();
			break;
		case 136:
			return size136_malloc();
			break;
		case 264:
			return size264_malloc();
			break;
		case 520:
			return size520_malloc();
			break;
		case 1032:
			return size1032_malloc();
			break;
		case 2056:
			return size2056_malloc();
			break;
		default:
			return pmalloc_helper(nbytes);
			break;
	}
}

void*
prealloc(void* prev, size_t nn)
{
  size_t *new = xmalloc(nn);
	size_t *ptr = (size_t*)prev-1;
	//printf("xrealloc(%ld, %ld)\n", *ptr, nn);
	
	memset(new, 0, nn);
	if (nn >= *ptr)
		memcpy(new, prev, *ptr-sizeof(size_t));
	
	xfree(prev);
	
	return new;
}


