#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/mmzone.h>
#include <linux/proc_fs.h>
#include <linux/quicklist.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/atomic.h>
#include <linux/vmalloc.h>
#ifdef CONFIG_CMA
#include <linux/cma.h>
#endif
#include <asm/page.h>
#include <asm/pgtable.h>
#include "internal.h"

#include <popcorn/remote_meminfo.h>

void __attribute__((weak)) arch_report_meminfo(struct seq_file *m)
{
}

static int meminfo_proc_show(struct seq_file *m, void *v)
{
	struct sysinfo i;
	unsigned long committed;
	long cached;
	long available;
	unsigned long pagecache;
	unsigned long wmark_low = 0;
	unsigned long pages[NR_LRU_LISTS];
	struct zone *zone;
	int lru;

	remote_mem_info_response_t rem_mem;

/*
 * display in kilobytes.
 */
#define K(x) ((x) << (PAGE_SHIFT - 10))
	si_meminfo(&i);
	si_swapinfo(&i);
	committed = percpu_counter_read_positive(&vm_committed_as);

	cached = global_page_state(NR_FILE_PAGES) -
			total_swapcache_pages() - i.bufferram;
	if (cached < 0)
		cached = 0;

	for (lru = LRU_BASE; lru < NR_LRU_LISTS; lru++)
		pages[lru] = global_page_state(NR_LRU_BASE + lru);

	for_each_zone(zone)
		wmark_low += zone->watermark[WMARK_LOW];

	/*
	 * Estimate the amount of memory available for userspace allocations,
	 * without causing swapping.
	 *
	 * Free memory cannot be taken below the low watermark, before the
	 * system starts swapping.
	 */
	available = i.freeram - wmark_low;

	/*
	 * Not all the page cache can be freed, otherwise the system will
	 * start swapping. Assume at least half of the page cache, or the
	 * low watermark worth of cache, needs to stay.
	 */
	pagecache = pages[LRU_ACTIVE_FILE] + pages[LRU_INACTIVE_FILE];
	pagecache -= min(pagecache / 2, wmark_low);
	available += pagecache;

	/*
	 * Part of the reclaimable slab consists of items that are in use,
	 * and cannot be freed. Cap this estimate at the low watermark.
	 */
	available += global_page_state(NR_SLAB_RECLAIMABLE) -
		     min(global_page_state(NR_SLAB_RECLAIMABLE) / 2, wmark_low);

	if (available < 0)
		available = 0;

	remote_proc_mem_info(&rem_mem);

	/*
	 * Tagged format, for easy grepping and expansion.
	 */
	seq_printf(m,
		"MemTotal:       %8lu kB\n"
		"MemFree:        %8lu kB\n"
		"MemAvailable:   %8lu kB\n"
		"Buffers:        %8lu kB\n"
		"Cached:         %8lu kB\n"
		"SwapCached:     %8lu kB\n"
		"Active:         %8lu kB\n"
		"Inactive:       %8lu kB\n"
		"Active(anon):   %8lu kB\n"
		"Inactive(anon): %8lu kB\n"
		"Active(file):   %8lu kB\n"
		"Inactive(file): %8lu kB\n"
		"Unevictable:    %8lu kB\n"
		"Mlocked:        %8lu kB\n"
#ifdef CONFIG_HIGHMEM
		"HighTotal:      %8lu kB\n"
		"HighFree:       %8lu kB\n"
		"LowTotal:       %8lu kB\n"
		"LowFree:        %8lu kB\n"
#endif
#ifndef CONFIG_MMU
		"MmapCopy:       %8lu kB\n"
#endif
		"SwapTotal:      %8lu kB\n"
		"SwapFree:       %8lu kB\n"
		"Dirty:          %8lu kB\n"
		"Writeback:      %8lu kB\n"
		"AnonPages:      %8lu kB\n"
		"Mapped:         %8lu kB\n"
		"Shmem:          %8lu kB\n"
		"Slab:           %8lu kB\n"
		"SReclaimable:   %8lu kB\n"
		"SUnreclaim:     %8lu kB\n"
		"KernelStack:    %8lu kB\n"
		"PageTables:     %8lu kB\n"
#ifdef CONFIG_QUICKLIST
		"Quicklists:     %8lu kB\n"
#endif
		"NFS_Unstable:   %8lu kB\n"
		"Bounce:         %8lu kB\n"
		"WritebackTmp:   %8lu kB\n"
		"CommitLimit:    %8lu kB\n"
		"Committed_AS:   %8lu kB\n"
		"VmallocTotal:   %8lu kB\n"
		"VmallocUsed:    %8lu kB\n"
		"VmallocChunk:   %8lu kB\n"
#ifdef CONFIG_MEMORY_FAILURE
		"HardwareCorrupted: %5lu kB\n"
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		"AnonHugePages:  %8lu kB\n"
#endif
#ifdef CONFIG_CMA
		"CmaTotal:       %8lu kB\n"
		"CmaFree:        %8lu kB\n"
#endif
		,
		K(i.totalram) + rem_mem.MemTotal,
		K(i.freeram) + rem_mem.MemFree,
		K(available) + rem_mem.MemAvailable,
		K(i.bufferram) + rem_mem.Buffers,
		K(cached) + rem_mem.Cached,
		K(total_swapcache_pages()) + rem_mem.SwapCached,
		K(pages[LRU_ACTIVE_ANON]   + pages[LRU_ACTIVE_FILE]) + rem_mem.Active,
		K(pages[LRU_INACTIVE_ANON] + pages[LRU_INACTIVE_FILE]) + rem_mem.Inactive,
		K(pages[LRU_ACTIVE_ANON]) + rem_mem.Active_anon,
		K(pages[LRU_INACTIVE_ANON]) + rem_mem.Inactive_anon,
		K(pages[LRU_ACTIVE_FILE]) + rem_mem.Active_file,
		K(pages[LRU_INACTIVE_FILE]) + rem_mem.Inactive_file,
		K(pages[LRU_UNEVICTABLE]) + rem_mem.Unevictable,
		K(global_page_state(NR_MLOCK)) + rem_mem.Mlocked,
#ifdef CONFIG_HIGHMEM
		K(i.totalhigh) + rem_mem.HighTotal,
		K(i.freehigh) + rem_mem.HighFree,
		K(i.totalram-i.totalhigh) + rem_mem.LowTotal,
		K(i.freeram-i.freehigh) + rem_mem.LowFree,
#endif
#ifndef CONFIG_MMU
		K((unsigned long) atomic_long_read(&mmap_pages_allocated)) + rem_mem.MmapCopy,
#endif
		K(i.totalswap) + rem_mem.SwapTotal,
		K(i.freeswap) + rem_mem.SwapFree,
		K(global_page_state(NR_FILE_DIRTY)) + rem_mem.Dirty,
		K(global_page_state(NR_WRITEBACK)) + rem_mem.Writeback,
		K(global_page_state(NR_ANON_PAGES)) + rem_mem.AnonPages,
		K(global_page_state(NR_FILE_MAPPED)) + rem_mem.Mapped,
		K(i.sharedram) + rem_mem.Shmem,
		K(global_page_state(NR_SLAB_RECLAIMABLE) +
				global_page_state(NR_SLAB_UNRECLAIMABLE)) + rem_mem.Slab,
		K(global_page_state(NR_SLAB_RECLAIMABLE)) + rem_mem.SReclaimable,
		K(global_page_state(NR_SLAB_UNRECLAIMABLE)) + rem_mem.SUnreclaim,
		global_page_state(NR_KERNEL_STACK) * THREAD_SIZE / 1024 + rem_mem.KernelStack,
		K(global_page_state(NR_PAGETABLE)) + rem_mem.PageTables,
#ifdef CONFIG_QUICKLIST
		K(quicklist_total_size()) + rem_mem.Quicklists,
#endif
		K(global_page_state(NR_UNSTABLE_NFS)) + rem_mem.NFS_Unstable,
		K(global_page_state(NR_BOUNCE)) + rem_mem.Bounce,
		K(global_page_state(NR_WRITEBACK_TEMP)) + rem_mem.WritebackTmp,
		K(vm_commit_limit()) + rem_mem.CommitLimit,
		K(committed) + rem_mem.Committed_AS,
		((unsigned long)VMALLOC_TOTAL >> 10) + rem_mem.VmallocTotal,
		0ul, // used to be vmalloc 'used'
		0ul  // used to be vmalloc 'largest_chunk'
#ifdef CONFIG_MEMORY_FAILURE
		, (atomic_long_read(&num_poisoned_pages) << (PAGE_SHIFT - 10)) + rem_mem.HardwareCorrupted
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		, K(global_page_state(NR_ANON_TRANSPARENT_HUGEPAGES) *
		   HPAGE_PMD_NR) + rem_mem.AnonHugePages
#endif
#ifdef CONFIG_CMA
		, K(totalcma_pages) + rem_mem.CmaTotal
		, K(global_page_state(NR_FREE_CMA_PAGES)) + rem_mem.CmaFree
#endif
		);

	hugetlb_report_meminfo(m);

	arch_report_meminfo(m);

	return 0;
#undef K
}

static int meminfo_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, meminfo_proc_show, NULL);
}

static const struct file_operations meminfo_proc_fops = {
	.open		= meminfo_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_meminfo_init(void)
{
	proc_create("meminfo", 0, NULL, &meminfo_proc_fops);
	return 0;
}
fs_initcall(proc_meminfo_init);
