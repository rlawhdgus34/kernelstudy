/*
 * sparse memory mappings.
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/bootmem.h>
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "internal.h"
#include <asm/dma.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

/*
 * Permanent SPARSEMEM data:
 *
 * 1) mem_section	- memory sections, mem_map's for valid memory
 */
#ifdef CONFIG_SPARSEMEM_EXTREME
struct mem_section *mem_section[NR_SECTION_ROOTS]
	____cacheline_internodealigned_in_smp;
#else
struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT]
	____cacheline_internodealigned_in_smp;
#endif
EXPORT_SYMBOL(mem_section);

#ifdef NODE_NOT_IN_PAGE_FLAGS
/*
 * If we did not store the node number in the page then we have to
 * do a lookup in the section_to_node_table in order to find which
 * node the page belongs to.
 */
#if MAX_NUMNODES <= 256
static u8 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#else
static u16 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#endif

int page_to_nid(const struct page *page)
{
	return section_to_node_table[page_to_section(page)];
}
EXPORT_SYMBOL(page_to_nid);

static void set_section_nid(unsigned long section_nr, int nid)
{
	section_to_node_table[section_nr] = nid;
}
#else /* !NODE_NOT_IN_PAGE_FLAGS */
static inline void set_section_nid(unsigned long section_nr, int nid)
{
}
#endif

#ifdef CONFIG_SPARSEMEM_EXTREME
static struct mem_section noinline __init_refok *sparse_index_alloc(int nid)
{
	struct mem_section *section = NULL;
	unsigned long array_size = SECTIONS_PER_ROOT *
				   sizeof(struct mem_section);

	if (slab_is_available()) {
		if (node_state(nid, N_HIGH_MEMORY))
			section = kzalloc_node(array_size, GFP_KERNEL, nid);
		else
			section = kzalloc(array_size, GFP_KERNEL);
	} else {
		section = memblock_virt_alloc_node(array_size, nid);
	}

	return section;
}

static int __meminit sparse_index_init(unsigned long section_nr, int nid)
{
	unsigned long root = SECTION_NR_TO_ROOT(section_nr);
	struct mem_section *section;

	if (mem_section[root])
		return -EEXIST;

	section = sparse_index_alloc(nid);
	if (!section)
		return -ENOMEM;

	mem_section[root] = section;

	return 0;
}
#else /* !SPARSEMEM_EXTREME */
static inline int sparse_index_init(unsigned long section_nr, int nid)
{
	return 0;
}
#endif

/*
 * Although written for the SPARSEMEM_EXTREME case, this happens
 * to also work for the flat array case because
 * NR_SECTION_ROOTS==NR_MEM_SECTIONS.
 */
int __section_nr(struct mem_section* ms)
{
	unsigned long root_nr;
	struct mem_section* root;

	for (root_nr = 0; root_nr < NR_SECTION_ROOTS; root_nr++) {
		root = __nr_to_section(root_nr * SECTIONS_PER_ROOT);
		if (!root)
			continue;

		if ((ms >= root) && (ms < (root + SECTIONS_PER_ROOT)))
		     break;
	}

	VM_BUG_ON(root_nr == NR_SECTION_ROOTS);

	return (root_nr * SECTIONS_PER_ROOT) + (ms - root);
}

/*
 * During early boot, before section_mem_map is used for an actual
 * mem_map, we use section_mem_map to store the section's NUMA
 * node.  This keeps us from having to use another data structure.  The
 * node information is cleared just before we store the real mem_map.
 */
static inline unsigned long sparse_encode_early_nid(int nid)
{
	return (nid << SECTION_NID_SHIFT);
}

static inline int sparse_early_nid(struct mem_section *section)
{
	return (section->section_mem_map >> SECTION_NID_SHIFT);
}

/* Validate the physical addressing limitations of the model */
void __meminit mminit_validate_memmodel_limits(unsigned long *start_pfn,
						unsigned long *end_pfn)
{
	unsigned long max_sparsemem_pfn = 1UL << (MAX_PHYSMEM_BITS-PAGE_SHIFT);

	/*
	 * Sanity checks - do not allow an architecture to pass
	 * in larger pfns than the maximum scope of sparsemem:
	 */
	if (*start_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"Start of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*start_pfn = max_sparsemem_pfn;
		*end_pfn = max_sparsemem_pfn;
	} else if (*end_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"End of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*end_pfn = max_sparsemem_pfn;
	}
}

/* Record a memory area against a node. */
void __init memory_present(int nid, unsigned long start, unsigned long end)
{
	//매개변수 nid는 0, 즉 0번 노드를 의미,
	unsigned long pfn;

	/*
	 * Flat memory 의 경우에는 mem_map 변수 하나만 이용하는데
	 * sparse memory 의 경우 
	 * usemap_map[]
	 * mem_section[]
	 * map_map[]
	 * section_to_node_table[]
	 * 이렇게 네개를 이용하고
	 * 이 함수에서는 mem_section 매핑을 위한 할당 수행
	 */
	//UMA 구조 사용시 flat 메모리만 사용 , NUMA 사용시 flat, sparse 둘다 사용
	//arm에서 NUMA의 특성을 연관지을 근거가 없음.(ex].arm으로 서버를 사용하지 않음)
	//그러나 최근에 arm에서도 최근 sparse mem 모델로 옮겨질수도 있음)
	start &= PAGE_SECTION_MASK;
	mminit_validate_memmodel_limits(&start, &end);
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		unsigned long section = pfn_to_section_nr(pfn);
		struct mem_section *ms;

		sparse_index_init(section, nid);
		set_section_nid(section, nid);

		ms = __nr_to_section(section);
		//mem_section[] 매핑을 위한 allocation 수행을 위한 영역 record
		if (!ms->section_mem_map)
			ms->section_mem_map = sparse_encode_early_nid(nid) |
							SECTION_MARKED_PRESENT;
	}
}

/*
 * Only used by the i386 NUMA architecures, but relatively
 * generic code.
 */
unsigned long __init node_memmap_size_bytes(int nid, unsigned long start_pfn,
						     unsigned long end_pfn)
{
	unsigned long pfn;
	unsigned long nr_pages = 0;

	mminit_validate_memmodel_limits(&start_pfn, &end_pfn);
	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		if (nid != early_pfn_to_nid(pfn))
			continue;

		if (pfn_present(pfn))
			nr_pages += PAGES_PER_SECTION;
	}

	return nr_pages * sizeof(struct page);
}

/*
 * Subtle, we encode the real pfn into the mem_map such that
 * the identity pfn - section_mem_map will return the actual
 * physical page frame number.
 */
static unsigned long sparse_encode_mem_map(struct page *mem_map, unsigned long pnum)
{
	return (unsigned long)(mem_map - (section_nr_to_pfn(pnum)));
}

/*
 * Decode mem_map from the coded memmap
 */
struct page *sparse_decode_mem_map(unsigned long coded_mem_map, unsigned long pnum)
{
	/* mask off the extra low bits of information */
	coded_mem_map &= SECTION_MAP_MASK;
	return ((struct page *)coded_mem_map) + section_nr_to_pfn(pnum);
}

static int __meminit sparse_init_one_section(struct mem_section *ms,
		unsigned long pnum, struct page *mem_map,
		unsigned long *pageblock_bitmap)
{
	if (!present_section(ms))
		return -EINVAL;

	// bit 연산 같지만, encoding 해서 mem_map 매다는 것, 나중에 디코딩해서 이용
	// sparse_encode_mem_map 함수에서 인코딩 (주소 - PFN)
	ms->section_mem_map &= ~SECTION_MAP_MASK;
	ms->section_mem_map |= sparse_encode_mem_map(mem_map, pnum) |
							SECTION_HAS_MEM_MAP;
 	ms->pageblock_flags = pageblock_bitmap; // usemap 매달기

	return 1;
}

unsigned long usemap_size(void)
{
	unsigned long size_bytes;
	size_bytes = roundup(SECTION_BLOCKFLAGS_BITS, 8) / 8;
	size_bytes = roundup(size_bytes, sizeof(unsigned long));
	return size_bytes;
}

#ifdef CONFIG_MEMORY_HOTPLUG
static unsigned long *__kmalloc_section_usemap(void)
{
	return kmalloc(usemap_size(), GFP_KERNEL);
}
#endif /* CONFIG_MEMORY_HOTPLUG */

#ifdef CONFIG_MEMORY_HOTREMOVE
static unsigned long * __init
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
	//가능하면 노드정보(pgdat)가 담겨있는 섹션에 usemap이 들어갈수 있도록 메모리 할당
	//노드에 속한 페이지를 관리하는 구조체.
{
	unsigned long goal, limit;
	unsigned long *p;
	int nid;
	/*
	 * A page may contain usemaps for other sections preventing the
	 * page being freed and making a section unremovable while
	 * other sections referencing the usemap remain active. Similarly,
	 * a pgdat can prevent a section being removed. If section A
	 * contains a pgdat and section B contains the usemap, both
	 * sections become inter-dependent. This allocates usemaps
	 * from the same section as the pgdat where possible to avoid
	 * this problem.

	 //섹션 단위로 지워지는 pgdat.
	 */
	goal = __pa(pgdat) & (PAGE_SECTION_MASK << PAGE_SHIFT);//pgdat가 담겨있는 섹션시작 물리주소
	limit = goal + (1UL << PA_SECTION_SHIFT);//섹션 크기 탐색
	nid = early_pfn_to_nid(goal >> PAGE_SHIFT);//pgdat가 있는 섹션의 노드id
again:
	p = memblock_virt_alloc_try_nid_nopanic(size,
						SMP_CACHE_BYTES, goal, limit,
						nid);
	//goal과 limit으로 지정된 범위의 섹션 영역 내에서 size만큼 memblock 공간 할당
	if (!p && limit) {//할당 실패,  limit이 0이 아닐떄
		limit = 0;
		goto again;
	}
	return p;
}

static void __init check_usemap_section_nr(int nid, unsigned long *usemap)
{
	unsigned long usemap_snr, pgdat_snr;
	static unsigned long old_usemap_snr = NR_MEM_SECTIONS;
	static unsigned long old_pgdat_snr = NR_MEM_SECTIONS;
	struct pglist_data *pgdat = NODE_DATA(nid);
	int usemap_nid;

	usemap_snr = pfn_to_section_nr(__pa(usemap) >> PAGE_SHIFT);
	pgdat_snr = pfn_to_section_nr(__pa(pgdat) >> PAGE_SHIFT);
	if (usemap_snr == pgdat_snr)
		return;

	if (old_usemap_snr == usemap_snr && old_pgdat_snr == pgdat_snr)
		/* skip redundant message */
		return;

	old_usemap_snr = usemap_snr;
	old_pgdat_snr = pgdat_snr;

	usemap_nid = sparse_early_nid(__nr_to_section(usemap_snr));
	if (usemap_nid != nid) {
		pr_info("node %d must be removed before remove section %ld\n",
			nid, usemap_snr);
		return;
	}
	/*
	 * There is a circular dependency.
	 * Some platforms allow un-removable section because they will just
	 * gather other removable sections for dynamic partitioning.
	 * Just notify un-removable section's number here.
	 */
	pr_info("Section %ld and %ld (node %d) have a circular dependency on usemap and pgdat allocations\n",
		usemap_snr, pgdat_snr, nid);
}
#else
static unsigned long * __init
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
{
	return memblock_virt_alloc_node_nopanic(size, pgdat->node_id);
}

static void __init check_usemap_section_nr(int nid, unsigned long *usemap)
{
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

static void __init sparse_early_usemaps_alloc_node(void *data,
				 unsigned long pnum_begin,
				 unsigned long pnum_end,
				 unsigned long usemap_count, int nodeid)
{
	void *usemap;
	unsigned long pnum;
	unsigned long **usemap_map = (unsigned long **)data;
	int size = usemap_size();//usemap_size를가져옴. 한 섹션에 들어갈 수 있는 페이지 블록 관리 구조체의 크기.

	usemap = sparse_early_usemaps_alloc_pgdat_section(NODE_DATA(nodeid),
							  size * usemap_count);
	//지정된 노드에 가능하면 노드 정보가 기록된 섹션에 usemap_count만큼 공간 할당.
	if (!usemap) {
		pr_warn("%s: allocation failed\n", __func__);
		return;
	}

	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		if (!present_section_nr(pnum))
			continue;
		usemap_map[pnum] = usemap;//usemap_map에 위 함수를 통해 할당된 공간의 주소 대입.
		usemap += size;//다음 대입을 위한 위치변경
		check_usemap_section_nr(nodeid, usemap_map[pnum]);
		//할당받은 usemap의 섹션이 노드정보가 기록된 섹션과 같지 않으면 경고
		//circular dependency ---> 노드와 섹션의 정보가 맞지 않을때. 
	}
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
struct page __init *sparse_mem_map_populate(unsigned long pnum, int nid)
{
	struct page *map;
	unsigned long size;

	map = alloc_remap(nid, sizeof(struct page) * PAGES_PER_SECTION);
	if (map)
		return map;

	size = PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION);
	map = memblock_virt_alloc_try_nid(size,
					  PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
					  BOOTMEM_ALLOC_ACCESSIBLE, nid);
	return map;
}
void __init sparse_mem_maps_populate_node(struct page **map_map,
					  unsigned long pnum_begin,
					  unsigned long pnum_end,
					  unsigned long map_count, int nodeid)
{
	void *map;
	unsigned long pnum;
	unsigned long size = sizeof(struct page) * PAGES_PER_SECTION;

	/*
	 * tile archtecture 의 경우 remap 함수가 할당되서 불리고
	 * 나머지의 경우 remap 함수는 NULL 리턴해서 아래 if 문 무시
	 */
	map = alloc_remap(nodeid, size * map_count); 
	if (map) { 
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			map_map[pnum] = map;
			map += size;
		}
		return;
	}

	size = PAGE_ALIGN(size); // 올림
	map = memblock_virt_alloc_try_nid(size * map_count,
					  PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
					  BOOTMEM_ALLOC_ACCESSIBLE, nodeid);
	if (map) {
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			map_map[pnum] = map;
			map += size;
		}
		return;
	}

	/* fallback */
	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;
		map_map[pnum] = sparse_mem_map_populate(pnum, nodeid);
		if (map_map[pnum])
			continue;
		ms = __nr_to_section(pnum);
		pr_err("%s: sparsemem memory map backing failed some memory will not be available\n",
		       __func__);
		ms->section_mem_map = 0;
	}
}
#endif /* !CONFIG_SPARSEMEM_VMEMMAP */

#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
static void __init sparse_early_mem_maps_alloc_node(void *data,
				 unsigned long pnum_begin,
				 unsigned long pnum_end,
				 unsigned long map_count, int nodeid)
{
	struct page **map_map = (struct page **)data;
	sparse_mem_maps_populate_node(map_map, pnum_begin, pnum_end,
					 map_count, nodeid);
}
#else
static struct page __init *sparse_early_mem_map_alloc(unsigned long pnum)
{
	struct page *map;
	struct mem_section *ms = __nr_to_section(pnum);
	int nid = sparse_early_nid(ms);

	map = sparse_mem_map_populate(pnum, nid);
	if (map)
		return map;

	pr_err("%s: sparsemem memory map backing failed some memory will not be available\n",
	       __func__);
	ms->section_mem_map = 0;
	return NULL;
}
#endif

void __weak __meminit vmemmap_populate_print_last(void)
{
}

/**
 *  alloc_usemap_and_memmap - memory alloction for pageblock flags and vmemmap
 *  @map: usemap_map for pageblock flags or mmap_map for vmemmap
 */
static void __init alloc_usemap_and_memmap(void (*alloc_func)
					(void *, unsigned long, unsigned long,
					unsigned long, int), void *data)
{
	//usemap과 mem_map에 대해 노드별 할당을 위해 시작섹션, 끝세션, hole을 제외한 섹션수를 파악하여 각각 할당 함수 호출.

	unsigned long pnum;
	unsigned long map_count;
	int nodeid_begin = 0;
	unsigned long pnum_begin = 0;

	for (pnum = 0; pnum < NR_MEM_SECTIONS; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;//mem_section이 매핑되어있지 않으면 섹션을 hole로 판단
		ms = __nr_to_section(pnum);
		nodeid_begin = sparse_early_nid(ms);
		pnum_begin = pnum;
		break;
	}//hole이 아닌 시작 세션을 파악하는 반복문.
	map_count = 1;
	for (pnum = pnum_begin + 1; pnum < NR_MEM_SECTIONS; pnum++) {
		struct mem_section *ms;
		int nodeid;

		if (!present_section_nr(pnum))
			continue;
		ms = __nr_to_section(pnum);
		nodeid = sparse_early_nid(ms);
		if (nodeid == nodeid_begin) {//mem_section에 노드와 현재 검토중인 mem_section이 같은 노드에 있을 경우
			map_count++;//한 노드안에 있는 섹션의 개수
			continue;
		}
		/* ok, we need to take care of from pnum_begin to pnum - 1*/
		alloc_func(data, pnum_begin, pnum,
						map_count, nodeid_begin);
		//data : 할당된 테이블.
		//두번째 반복문으로 도출된 pnum 인덱스까지 alloc_func을 수행.
		//sparse_early_usemaps_alloc_node 콜백 호출
		//--->
		/* new start, update count etc*/
		nodeid_begin = nodeid;
		pnum_begin = pnum;
		map_count = 1;
		//노드 시작번호를 현재 노드번호로 바꿔주고, 섹션 인덱스도 현재 인덱스로 변경, 섹션 개수도 1개로 설정, 이미 이 시점에서 섹션이 하나 설정 되어있음을 알수 있음.
	}
	/* ok, last chunk */
	alloc_func(data, pnum_begin, NR_MEM_SECTIONS,
						map_count, nodeid_begin);
}

/*
 * Allocate the accumulated non-linear sections, allocate a mem_map
 * for each and record the physical to section mapping.
 */
/*
 * memory 모델을 sparse model 을 이용할 때 usemap_map, map_map 을 할당
 * 매핑 초기화
 */
void __init sparse_init(void)
{
	//0번노드에서 묶여있다가 나뉘어진구간의  노드별의 usemap을 할당하고 usemap_map에매핑.
	unsigned long pnum;
	struct page *map;     //상대적으로 사이즈가 큼.(2메가바이트)
	unsigned long *usemap;//상대적으로 사이즈가 작음(24바이트)
	unsigned long **usemap_map;
	int size;
#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	//이 경우 노드별 mem_map을 할당, map_map에 매핑.
	int size2;
	struct page **map_map;
#endif

	/* see include/linux/mmzone.h 'struct mem_section' definition */
	BUILD_BUG_ON(!is_power_of_2(sizeof(struct mem_section)));

	/* Setup pageblock_order for HUGETLB_PAGE_SIZE_VARIABLE */
	set_pageblock_order();
	//페이지 블록 단위 설정.

	/*
	 * map is using big page (aka 2M in x86 64 bit)
	 * usemap is less one page (aka 24 bytes)
	 * so alloc 2M (with 2M align) and 24 bytes in turn will
	 * make next 2M slip to one more 2M later.
	 * then in big system, the memory will have a lot of holes...
	 * here try to allocate 2M pages continuously.
	 *
	 * powerpc need to call sparse_init_one_section right after each
	 * sparse_early_mem_map_alloc, so allocate usemap_map at first.
	 */
	size = sizeof(unsigned long *) * NR_MEM_SECTIONS;
	//포인터 테이블의 크기를 지정.
	usemap_map = memblock_virt_alloc(size, 0);
	//usemap 배열용 메모리 할당.
	if (!usemap_map)
		panic("can not allocate usemap_map\n");
	alloc_usemap_and_memmap(sparse_early_usemaps_alloc_node,
							(void *)usemap_map);
	//노드에 대한 usemap 할당, 그 usemap을 usemap_map[]에 매핑.

#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	size2 = sizeof(struct page *) * NR_MEM_SECTIONS;
	map_map = memblock_virt_alloc(size2, 0);
	if (!map_map)
		panic("can not allocate map_map\n");
	alloc_usemap_and_memmap(sparse_early_mem_maps_alloc_node,
							(void *)map_map);
#endif

	for (pnum = 0; pnum < NR_MEM_SECTIONS; pnum++) {
		if (!present_section_nr(pnum))
			continue;

		usemap = usemap_map[pnum];
		if (!usemap)
			continue;

#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
		map = map_map[pnum];
#else
		map = sparse_early_mem_map_alloc(pnum);
#endif
		if (!map)
			continue;

		/*
		 * usemap map 은 섹션단위로 되어 있고
		 * struct mem_section 은 섹션들을 관리하기 위한 정보들을 갖고 있음
		 * sparse_init_one_section 에서 해당 섹션에 대한 usemap과 map들의
		 * 정보를 링크
		 */
		sparse_init_one_section(__nr_to_section(pnum), pnum, map,
								usemap);
	}

	vmemmap_populate_print_last();

// struct mem_section 에 전부 다 매달아 놨으니 지역변수들 free
#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	memblock_free_early(__pa(map_map), size2);
#endif
	memblock_free_early(__pa(usemap_map), size);
}

#ifdef CONFIG_MEMORY_HOTPLUG
#ifdef CONFIG_SPARSEMEM_VMEMMAP
static inline struct page *kmalloc_section_memmap(unsigned long pnum, int nid)
{
	/* This will make the necessary allocations eventually. */
	return sparse_mem_map_populate(pnum, nid);
}
static void __kfree_section_memmap(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end);
}
#ifdef CONFIG_MEMORY_HOTREMOVE
static void free_map_bootmem(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end);
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#else
static struct page *__kmalloc_section_memmap(void)
{
	struct page *page, *ret;
	unsigned long memmap_size = sizeof(struct page) * PAGES_PER_SECTION;

	page = alloc_pages(GFP_KERNEL|__GFP_NOWARN, get_order(memmap_size));
	if (page)
		goto got_map_page;

	ret = vmalloc(memmap_size);
	if (ret)
		goto got_map_ptr;

	return NULL;
got_map_page:
	ret = (struct page *)pfn_to_kaddr(page_to_pfn(page));
got_map_ptr:

	return ret;
}

static inline struct page *kmalloc_section_memmap(unsigned long pnum, int nid)
{
	return __kmalloc_section_memmap();
}

static void __kfree_section_memmap(struct page *memmap)
{
	if (is_vmalloc_addr(memmap))
		vfree(memmap);
	else
		free_pages((unsigned long)memmap,
			   get_order(sizeof(struct page) * PAGES_PER_SECTION));
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static void free_map_bootmem(struct page *memmap)
{
	unsigned long maps_section_nr, removing_section_nr, i;
	unsigned long magic, nr_pages;
	struct page *page = virt_to_page(memmap);

	nr_pages = PAGE_ALIGN(PAGES_PER_SECTION * sizeof(struct page))
		>> PAGE_SHIFT;

	for (i = 0; i < nr_pages; i++, page++) {
		magic = (unsigned long) page->lru.next;

		BUG_ON(magic == NODE_INFO);

		maps_section_nr = pfn_to_section_nr(page_to_pfn(page));
		removing_section_nr = page->private;

		/*
		 * When this function is called, the removing section is
		 * logical offlined state. This means all pages are isolated
		 * from page allocator. If removing section's memmap is placed
		 * on the same section, it must not be freed.
		 * If it is freed, page allocator may allocate it which will
		 * be removed physically soon.
		 */
		if (maps_section_nr != removing_section_nr)
			put_page_bootmem(page);
	}
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#endif /* CONFIG_SPARSEMEM_VMEMMAP */

/*
 * returns the number of sections whose mem_maps were properly
 * set.  If this is <=0, then that means that the passed-in
 * map was not consumed and must be freed.
 */
int __meminit sparse_add_one_section(struct zone *zone, unsigned long start_pfn)
{
	unsigned long section_nr = pfn_to_section_nr(start_pfn);
	struct pglist_data *pgdat = zone->zone_pgdat;
	struct mem_section *ms;
	struct page *memmap;
	unsigned long *usemap;
	unsigned long flags;
	int ret;

	/*
	 * no locking for this, because it does its own
	 * plus, it does a kmalloc
	 */
	ret = sparse_index_init(section_nr, pgdat->node_id);
	if (ret < 0 && ret != -EEXIST)
		return ret;
	memmap = kmalloc_section_memmap(section_nr, pgdat->node_id);
	if (!memmap)
		return -ENOMEM;
	usemap = __kmalloc_section_usemap();
	if (!usemap) {
		__kfree_section_memmap(memmap);
		return -ENOMEM;
	}

	pgdat_resize_lock(pgdat, &flags);

	ms = __pfn_to_section(start_pfn);
	if (ms->section_mem_map & SECTION_MARKED_PRESENT) {
		ret = -EEXIST;
		goto out;
	}

	memset(memmap, 0, sizeof(struct page) * PAGES_PER_SECTION);

	ms->section_mem_map |= SECTION_MARKED_PRESENT;

	ret = sparse_init_one_section(ms, section_nr, memmap, usemap);

out:
	pgdat_resize_unlock(pgdat, &flags);
	if (ret <= 0) {
		kfree(usemap);
		__kfree_section_memmap(memmap);
	}
	return ret;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
#ifdef CONFIG_MEMORY_FAILURE
static void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
	int i;

	if (!memmap)
		return;

	for (i = 0; i < nr_pages; i++) {
		if (PageHWPoison(&memmap[i])) {
			atomic_long_sub(1, &num_poisoned_pages);
			ClearPageHWPoison(&memmap[i]);
		}
	}
}
#else
static inline void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
}
#endif

static void free_section_usemap(struct page *memmap, unsigned long *usemap)
{
	struct page *usemap_page;

	if (!usemap)
		return;

	usemap_page = virt_to_page(usemap);
	/*
	 * Check to see if allocation came from hot-plug-add
	 */
	if (PageSlab(usemap_page) || PageCompound(usemap_page)) {
		kfree(usemap);
		if (memmap)
			__kfree_section_memmap(memmap);
		return;
	}

	/*
	 * The usemap came from bootmem. This is packed with other usemaps
	 * on the section which has pgdat at boot time. Just keep it as is now.
	 */

	if (memmap)
		free_map_bootmem(memmap);
}

void sparse_remove_one_section(struct zone *zone, struct mem_section *ms,
		unsigned long map_offset)
{
	struct page *memmap = NULL;
	unsigned long *usemap = NULL, flags;
	struct pglist_data *pgdat = zone->zone_pgdat;

	pgdat_resize_lock(pgdat, &flags);
	if (ms->section_mem_map) {
		usemap = ms->pageblock_flags;
		memmap = sparse_decode_mem_map(ms->section_mem_map,
						__section_nr(ms));
		ms->section_mem_map = 0;
		ms->pageblock_flags = NULL;
	}
	pgdat_resize_unlock(pgdat, &flags);

	clear_hwpoisoned_pages(memmap + map_offset,
			PAGES_PER_SECTION - map_offset);
	free_section_usemap(memmap, usemap);
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#endif /* CONFIG_MEMORY_HOTPLUG */
