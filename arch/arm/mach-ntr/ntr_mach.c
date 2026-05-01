#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/genalloc.h>
#include <asm/cp15.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/time.h>

#include <mach/tcm.h>

/**
 * Using this configuration the mappings are:
 * - ITCM: 0x0000_0000 - 0x000f_ffff (1M, mirrored every 32K)
 * - DTCM: 0x0010_0000 - 0x0010_3fff (16K, not mirrored)
 * 
 * Requires 3 MPU entries:
 * - @ 0x0000_0000 -  4K - Privileged only, executable, no read, no write
 * - @ 0x000f_8000 - 32K - Privileged only, executable, readable, writable
 * - @ 0x0010_3fff - 16K - Privileged only, no execute, readable, writable
 */
#define ITCM_ADDRESS	0x00000000
#define ITCM_SIZE	32768
#define ITCM_CONTROL	(ITCM_ADDRESS | 0b010110)
#define ITCM_MIRROR	0x000F8000

#define DTCM_ADDRESS	0x00100000
#define DTCM_SIZE	16384
#define DTCM_CONTROL	(DTCM_ADDRESS | 0b001010)

#define EVT_SIZE	(2 * 4 * 8) /* a pair of words per vector */

/** ITCM and DTCM genalloc pools */
#define ITCM_POOL	(ITCM_MIRROR + EVT_SIZE)
#define ITCM_POOL_SIZE	(ITCM_SIZE - EVT_SIZE)

#define DTCM_POOL	DTCM_ADDRESS
#define DTCM_POOL_SIZE	DTCM_SIZE

static void __init ntr_vector_fixup(unsigned long vector_base) {
	/**
	 * The Nintendo DS(i) systems don't have a way to set the
	 * vectors at addresses 0x0000_0000 or 0xffff_0000, and the
	 * provided redirection is limited to IRQs and some exceptions (no SWI).
	 *
	 * What we do instead is map the 32K ITCM at address 0x0000_0000 and install
	 * a minimal set of vector handlers that branch to those defined in vector_base.
	 *
	 * The remaining ITCM and DTCM are put in a genalloc pool to be used by drivers.
	 */
	u32 cr;
	unsigned long flags;
	local_irq_save(flags);

	cr = get_cr();
	set_cr(cr & ~(CR_DT | CR_IT));	/* disable the TCMs as they will be reconfigured */
	__asm__ volatile(
		"mcr p15, 0, %0, c9, c1, 0\n\t"	/* write data tightly-coupled memory */
		"mcr p15, 0, %1, c9, c1, 1\n\t" /* write instruction tightly-coupled memory */
		:: "r"(DTCM_CONTROL), "r"(ITCM_CONTROL) : "memory"
	);
	set_cr(cr | CR_IT | CR_DT);	/* enable the TCMs  */

	/* fill up the vector table in the ITCM so that upon entering
	 * the i-th vector we branch to vector_base + (4 * i):
	 * - each of the 8 vectors contains an `ldr pc, [pc, #24]` (PC := [PC + (8*4) - 8])
	 * - each of the 8 default handlers point to vector_base + (4*i)
	 */
	for (unsigned e = 0; e < 8; e++) {
		volatile u32 *evt = (volatile u32*)(ITCM_ADDRESS);
		evt[e] = 0xe59ff018;
		evt[e + 8] = vector_base + (4 * e);
	}

	local_irq_restore(flags);
}

/** Tightly-coupled memory management
 *  Can't use HAVE_TCM because:
 *   - the arm code assumes an MMU is present _or_ the ITCM can be mapped at a non-zero address
 *   - some memory at the start of the ITCM needs to be reserved for the exception vector table
*/
static struct gen_pool *ntr_alloc_tcm_genpool(unsigned order, unsigned long addr, size_t len) {
	struct gen_pool *pool = gen_pool_create(order, -1);
	BUG_ON(pool == NULL);
	int res = gen_pool_add(pool, addr, len, -1);
	BUG_ON(res != 0);
	return pool;
}

static struct gen_pool *ntr_get_tcm(unsigned tcm) {
	struct gen_pool *pool;
	static DEFINE_MUTEX(tcm_lock);
	static struct gen_pool *pools[2] = {}; /* 0 = itcm, 1 = dtcm */

	pool = pools[tcm];
	if (pool) /* optimistic non-locking path */
		return pool;

	mutex_lock(&tcm_lock);
	pool = pools[tcm];	/* recheck in case multiple threads waited for the lock */
	if (!pool) {		/* we're the first ones to get the lock, lazy init the pools */
		pools[0] = ntr_alloc_tcm_genpool(4, ITCM_POOL, ITCM_POOL_SIZE);
		printk("ITCM: Reserved [%px-%px] (%d bytes)\n", (void*)ITCM_POOL, (void*)(ITCM_POOL + ITCM_POOL_SIZE - 1), ITCM_POOL_SIZE);
		pools[1] = ntr_alloc_tcm_genpool(2, DTCM_POOL, DTCM_POOL_SIZE);
		printk("DTCM: Reserved [%px-%px] (%d bytes)\n", (void*)DTCM_POOL, (void*)(DTCM_POOL + DTCM_POOL_SIZE - 1), DTCM_POOL_SIZE);
		pool = pools[tcm];
	}
	mutex_unlock(&tcm_lock);
	return pool;
}

void *ntr_alloc_itcm(size_t len) {
	return (void*)gen_pool_alloc(ntr_get_tcm(0), len);
}

void ntr_free_itcm(void *ptr, size_t len) {
	gen_pool_free(ntr_get_tcm(0), (unsigned long)ptr, len);
}

void *ntr_alloc_dtcm(size_t len) {
	return (void*)gen_pool_alloc(ntr_get_tcm(1), len);
}

void ntr_free_dtcm(void *ptr, size_t len) {
	gen_pool_free(ntr_get_tcm(1), (unsigned long)ptr, len);
}

static void __init ntr_reserve(void) {
}

static void __init ntr_early_init(void) {
}

static void __init ntr_machine_init(void) {
}

static const char * const ntr_of_match[] __initconst = {
	"nintendo,ntr",
	NULL
};

DT_MACHINE_START(NINTENDO_DS, "Nintendo DS (NTR)")
	.dt_compat	= ntr_of_match,
	.reserve	= ntr_reserve,
	.init_early	= ntr_early_init,
	.init_machine	= ntr_machine_init,
	.irq_vector_fixup	= ntr_vector_fixup,
MACHINE_END

static const char * const twl_of_match[] __initconst = {
	"nintendo,twl",
	NULL,
};

DT_MACHINE_START(NINTENDO_DSI, "Nintendo DSi (TWL)")
	.dt_compat	= twl_of_match,
	.reserve	= ntr_reserve,
	.init_early	= ntr_early_init,
	.init_machine	= ntr_machine_init,
	.irq_vector_fixup	= ntr_vector_fixup,
MACHINE_END
