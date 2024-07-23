/*
* cycx_drv.c	cycx Support Module.
*
*		This module is a library of common hardware-specific
*		functions used by all Cyclades sync and some async (8x & 16x)
*		drivers.
*
* Copyright:	(c) 1998, 1999 Arnaldo Carvalho de Melo <acme@conectiva.com.br>
*
* Author:	Arnaldo Carvalho de Melo <acme@conectiva.com.br>
*
* Based on sdladrv.c by Gene Kozin <genek@compuserve.com>
*
*		This program is free software; you can redistribute it and/or
*		modify it under the terms of the GNU General Public License
*		as published by the Free Software Foundation; either version
*		2 of the License, or (at your option) any later version.
* ============================================================================
* 1999/05/28    acme		cycx_intack & cycx_intde gone for good
* 1999/05/18	acme		lots of unlogged work, submitting to Linus...
* 1999/01/03	acme		more judicious use of data types
* 1999/01/03	acme		judicious use of data types :>
*				cycx_inten trying to reset pending interrupts
*				from cyclom 2x - I think this isn't the way to
*				go, but for now...
* 1999/01/02	acme		cycx_intack ok, I think there's nothing to do
*				to ack an int in cycx_drv.c, only handle it in
*				cyx_isr (or in the other protocols: cyp_isr,
*				cyf_isr, when they get implemented.
* Dec 31, 1998	Arnaldo         cycx_data_boot & cycx_code_boot fixed, crossing
*				fingers to see x25_configure in cycx_x25.c
*				work... :)
* Dec 26, 1998	Arnaldo         load implementation fixed, seems to work! :)
*				cycx_2x_dpmbase_options with all the possible
*				DPM addresses (20).
*				cycx_intr implemented (test this!)
*				general code cleanup
* Dec  8, 1998	Ivan Passos	Cyclom-2X firmware load implementation.
* Aug  8, 1998	Arnaldo		Initial version.
*/

#ifdef MODULE
#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif
#include <linux/module.h>
#else
#define EXPORT_SYMBOL(function)
#endif
#include <linux/kernel.h>	/* printk(), and other useful stuff */
#include <linux/stddef.h>	/* offsetof(), etc. */
#include <linux/errno.h>	/* return codes */
#include <linux/sched.h>	/* for jiffies, HZ, etc. */
#include <linux/cycx_drv.h>	/* API definitions */
#include <linux/cycx_cfm.h>	/* CYCX firmware module definitions */
#include <linux/delay.h>	/* udelay */
#include <asm/io.h>		/* for inb(), outb(), etc. */

#define	MOD_VERSION	0
#define	MOD_RELEASE	1

#ifdef MODULE
MODULE_AUTHOR("Arnaldo Carvalho de Melo");
MODULE_DESCRIPTION("Cyclades Sync Cards Driver.");
#endif

/* Function Prototypes */
/* Module entry points. These are called by the OS and must be public. */
int init_module (void);
void cleanup_module (void);

/* Hardware-specific functions */
static int cycx_detect (cycxhw_t *hw);
static int cycx_load (cycxhw_t *hw, cfm_t *cfm, u32 len);
static int cycx_init (cycxhw_t *hw);
static int cycx_reset (cycxhw_t *hw);
static void cycx_bootcfg (cycxhw_t *hw);

static int init_cycx_2x (cycxhw_t *hw);
static int reset_cycx_2x (u32 addr);
static int detect_cycx_2x (u32 addr);

/* Miscellaneous functions */
static void delay_cycx (int sec);
static int get_option_index (u32 *optlist, u32 optval);
static u16 checksum (u8 *buf, u32 len);

#define wait_cyc(addr) cycx_exec(addr + CMD_OFFSET)

/* Global Data
 * Note: All data must be explicitly initialized!!! */

/* private data */
static char modname[] = "cycx_drv";
static char fullname[] = "Cyclom X Support Module";
static char copyright[]	= "(c) 1998, 1999 Arnaldo Carvalho de Melo";

/* Hardware configuration options.
 * These are arrays of configuration options used by verification routines.
 * The first element of each array is its size (i.e. number of options).
 */
static u32 cycx_2x_dpmbase_options[] =
{
	20,
	0xA0000, 0xA4000, 0xA8000, 0xAC000, 0xB0000, 0xB4000, 0xB8000,
	0xBC000, 0xC0000, 0xC4000, 0xC8000, 0xCC000, 0xD0000, 0xD4000,
	0xD8000, 0xDC000, 0xE0000, 0xE4000, 0xE8000, 0xEC000
};

static u32 cycx_2x_irq_options[]  = { 7, 3, 5, 9, 10, 11, 12, 15 };

/* Kernel Loadable Module Entry Points */
/* Module 'insert' entry point.
 * o print announcement
 * o initialize static data
 *
 * Return:	0	Ok
 *		< 0	error.
 * Context:	process */
#ifdef MODULE
int init_module (void)
{
	printk(KERN_INFO "%s v%u.%u %s\n",
		fullname, MOD_VERSION, MOD_RELEASE, copyright);
	printk(KERN_INFO "version=0x%X\n", LINUX_VERSION_CODE);
	return 0;
}
/* Module 'remove' entry point.
 * o release all remaining system resources */
void cleanup_module (void)
{
}
#endif
/* Kernel APIs */
/* Set up adapter.
 * o detect adapter type
 * o verify hardware configuration options
 * o check for hardware conflicts
 * o set up adapter shared memory
 * o test adapter memory
 * o load firmware
 * Return:	0	ok.
 *		< 0	error */
EXPORT_SYMBOL(cycx_setup);
int cycx_setup (cycxhw_t *hw, void *cfm, u32 len)
{
	u32 *irq_opt = NULL;	/* IRQ options */
	u32 *dpmbase_opt = NULL;/* DPM window base options */
	int err = 0;

	if (cycx_detect(hw)) {
		printk(KERN_ERR "%s: adapter Cyclom %uX not found at "
				"address 0x%lX!\n",
			modname, hw->type, (unsigned long) hw->dpmbase);
		return -EINVAL;
	}

	printk(KERN_INFO "%s: found Cyclom %uX card at address 0x%lx.\n",
			 modname, hw->type, (unsigned long) hw->dpmbase);

	switch (hw->type) {
		case CYCX_2X:
			irq_opt	= cycx_2x_irq_options;
			dpmbase_opt = cycx_2x_dpmbase_options;
			break;
		default:
			printk(KERN_ERR "%s: unknown card.\n", modname);
			return -EINVAL;
	}

	/* Verify IRQ configuration options */
	if (!get_option_index(irq_opt, hw->irq)) {
		printk (KERN_ERR "%s: IRQ %d is illegal!\n", modname, hw->irq);
		return -EINVAL;
	} 

	/* Setup adapter dual-port memory window and test memory */
	if (!hw->dpmbase) {
		printk(KERN_ERR "%s: you must specify the dpm address!\n",
				modname);
 		return -EINVAL;
	}
	else if (!get_option_index(dpmbase_opt, hw->dpmbase)) {
		printk(KERN_ERR "%s: memory address 0x%lX is illegal!\n",
				modname, (unsigned long) hw->dpmbase);
		return -EINVAL;
	}

	hw->dpmsize = CYCX_WINDOWSIZE;
	/* FIXME! Is this the only amount ever available? */
	hw->memory = 0x40000;

	cycx_init(hw);

	printk(KERN_INFO "%s: dual-port memory window is set at 0x%lX.\n",
			 modname, (unsigned long) hw->dpmbase);
	printk(KERN_INFO "%s: found %luK bytes of on-board memory.\n",
			 modname, (unsigned long) hw->memory / 1024);

	/* Load firmware. If loader fails then shut down adapter */
	err = cycx_load(hw, cfm, len);
	if (err) cycx_down(hw);		/* shutdown adapter */
	return err;
} 

/* Shut down CYCX: disable shared memory access and interrupts, stop CPU,etc.*/ 
EXPORT_SYMBOL(cycx_down);
int cycx_down (cycxhw_t *hw)
{
	return 0; /* FIXME: anything needed here? */
}

/* Enable interrupt generation.  */
EXPORT_SYMBOL(cycx_inten);
int cycx_inten (cycxhw_t *hw)
{
	switch (hw->type) {
		case CYCX_2X: writeb (0, hw->dpmbase); break;
		default: return -EINVAL;
	}

	return 0;
}

/* Generate an interrupt to adapter's CPU. */
EXPORT_SYMBOL(cycx_intr);
int cycx_intr (cycxhw_t *hw)
{
	switch (hw->type) {
		case CYCX_2X:
			writew(0, hw->dpmbase + GEN_CYCX_INTR);
			return 0;
		default: return -EINVAL;
	}

	return 0;
}

/* Execute Adapter Command.
 * o Set exec flag.
 * o Busy-wait until flag is reset. */
EXPORT_SYMBOL(cycx_exec);
int cycx_exec (u32 addr)
{
	u16 i = 0;
	/* wait till addr content is zeroed */

	while (readw(addr) != 0) {
		udelay(1000);
		if (++i > 50) return -1;
	}

	return 0;
}

/* Read absolute adapter memory.
 * Transfer data from adapter's memory to data buffer. */
EXPORT_SYMBOL(cycx_peek);
int cycx_peek (cycxhw_t *hw, u32 addr, void *buf, u32 len)
{
	if (len == 1)	*(u8*)buf = readb (hw->dpmbase + addr);
	else 		memcpy_fromio(buf, hw->dpmbase + addr, len);

	return 0;
}

/* Write Absolute Adapter Memory.
 * Transfer data from data buffer to adapter's memory. */
EXPORT_SYMBOL(cycx_poke);
int cycx_poke (cycxhw_t *hw, u32 addr, void *buf, u32 len)
{
	if (len == 1) writeb (*(u8*)buf, hw->dpmbase + addr);
	else	      memcpy_toio(hw->dpmbase + addr, buf, len);

	return 0;
}

/* Hardware-Specific Functions */
/* Detect adapter type.
 * o if adapter type is specified then call detection routine for that adapter
 *   type.  Otherwise call detection routines for every adapter types until
 *   adapter is detected.
 *
 * Notes:
 * 1) Detection tests are destructive! Adapter will be left in shutdown state
 *    after the test. */
static int cycx_detect (cycxhw_t *hw)
{
	int err = 0;

	if (!hw->dpmbase) return -EFAULT;

	switch (hw->type) {
		case CYCX_2X:
			if (!detect_cycx_2x(hw->dpmbase)) err = -ENODEV;
			break;
		default:
			if (detect_cycx_2x(hw->dpmbase)) hw->type = CYCX_2X;
			else err = -ENODEV;
	}

	return err;
}

/* Load Aux Routines */
/* Reset board hardware.
   return 1 if memory exists at addr and 0 if not. */
static int memory_exists(u32 addr)
{
	int timeout = 0;

	for (; timeout < 3 ; timeout++) {
		writew (TEST_PATTERN, addr + 0x10);

		if (readw (addr + 0x10) == TEST_PATTERN)
			if (readw (addr + 0x10) == TEST_PATTERN) return 1;

		delay_cycx(1);
	}

	return 0;
}

/* Reset board hardware. */
static int cycx_reset(cycxhw_t *hw)
{
	/* Reset board */
	switch (hw->type) {
		case CYCX_2X: return reset_cycx_2x(hw->dpmbase);
	}

	return -EINVAL;
}

/* Load reset code. */
static void reset_load(u32 addr, u8 *buffer, u32 cnt)
{
	u32 pt_code = addr + RESET_OFFSET;
	u16 i, j;

	for ( i = 0 ; i < cnt ; i++) {
		for (j = 0 ; j < 50 ; j++); /* Delay - FIXME busy waiting... */
		writeb(*buffer++, pt_code++);
	}
}

/* Load buffer using boot interface.
 * o copy data from buffer to Cyclom-X memory
 * o wait for reset code to copy it to right portion of memory */
static int buffer_load(u32 addr, u8 *buffer, u32 cnt)
{
	memcpy_toio(addr + DATA_OFFSET, buffer, cnt);
	writew(GEN_BOOT_DAT, addr + CMD_OFFSET);
	return wait_cyc(addr);
}

/* Set up entry point and kick start Cyclom-X CPU. */
static void cycx_start (u32 addr)
{
	/* put in 0x30 offset the jump instruction to the code entry point */
	writeb(0xea, addr + 0x30);
	writeb(0x00, addr + 0x31);
	writeb(0xc4, addr + 0x32);
	writeb(0x00, addr + 0x33);
	writeb(0x00, addr + 0x34);

	/* cmd to start executing code */
	writew(GEN_START, addr + CMD_OFFSET);
}         

/* Load and boot reset code. */
static void cycx_reset_boot(u32 addr, u8 *code, u32 len)
{
	u32 pt_start = addr + START_OFFSET;

        writeb(0xea, pt_start++); /* jmp to f000:3f00 */
        writeb(0x00, pt_start++);
        writeb(0xfc, pt_start++);
        writeb(0x00, pt_start++);
        writeb(0xf0, pt_start);
	reset_load(addr, code, len);

	/* 80186 was in hold, go */
	writeb(0, addr + START_CPU);
	delay_cycx(1);
}

/* Load data.bin file through boot (reset) interface. */
static int cycx_data_boot(u32 addr, u8 *code, u32 len)
{
	u32 pt_boot_cmd = addr + CMD_OFFSET;
	u32 i;

	/* boot buffer lenght */
	writew(CFM_LOAD_BUFSZ, pt_boot_cmd + sizeof(u16));
	writew(GEN_DEFPAR, pt_boot_cmd);

	if (wait_cyc(addr) < 0) return 2;

	writew(0, pt_boot_cmd + sizeof(u16));
	writew(0x4000, pt_boot_cmd + 2 * sizeof(u16));
	writew(GEN_SET_SEG, pt_boot_cmd);

	if (wait_cyc(addr) < 0) return 2;

	for (i = 0 ; i < len ; i += CFM_LOAD_BUFSZ)
		if (buffer_load(addr, code + i,
				MIN(CFM_LOAD_BUFSZ, (len - i))) < 0) {
			printk(KERN_ERR "%s: Error !!\n", modname);
			return 4;
		}

	return 0;
}


/* Load code.bin file through boot (reset) interface. */
static int cycx_code_boot(u32 addr, u8 *code, u32 len)
{
	u32 pt_boot_cmd = addr + CMD_OFFSET;
	u32 i;

	/* boot buffer lenght */
	writew(CFM_LOAD_BUFSZ, pt_boot_cmd + sizeof(u16));
	writew(GEN_DEFPAR, pt_boot_cmd);

	if (wait_cyc(addr) == -1) return 2;

	writew(0x0000, pt_boot_cmd + sizeof(u16));
	writew(0xc400, pt_boot_cmd + 2 * sizeof(u16));
	writew(GEN_SET_SEG, pt_boot_cmd);

	if (wait_cyc(addr) == -1) return 1;

	for (i = 0 ; i < len ; i += CFM_LOAD_BUFSZ)
		if (buffer_load(addr, code + i,MIN(CFM_LOAD_BUFSZ,(len - i)))) {
			printk(KERN_ERR "%s: Error !!\n", modname);
			return 4;
		}

	return 0;
}

/* Initialize CYCX hardware: setup memory window, IRQ, etc. */
static int cycx_init (cycxhw_t *hw)
{
	switch (hw->type) {
		case CYCX_2X: return init_cycx_2x(hw);
	}

	return -EINVAL;
}

/* Load adapter from the memory image of the CYCX firmware module. 
 * o verify firmware integrity and compatibility
 * o start adapter up */
static int cycx_load (cycxhw_t *hw, cfm_t *cfm, u32 len)
{
	int i, j, status;
	cycx_header_t *img_hdr;
	u8 *reset_image,
	   *data_image,
	   *code_image;
	u32 pt_cycld = hw->dpmbase + 0x400;
	u16 cksum;

	/* Announce */
	printk(KERN_INFO "%s: firmware signature=\"%s\"\n",
			 modname, cfm->signature); 

	/* Verify firmware signature */
	if (strcmp(cfm->signature, CFM_SIGNATURE)) {
		printk(KERN_ERR "%s:cycx_load: not Cyclom-2X firmware!\n",
				modname);
		return -EINVAL;
	}

	printk(KERN_INFO "%s: firmware version=%u\n", modname, cfm->version);

	/* Verify firmware module format version */
	if (cfm->version != CFM_VERSION) {
		printk(KERN_ERR "%s:cycx_load: firmware format %u rejected! "
				"Expecting %u.\n",
				modname, cfm->version, CFM_VERSION);
		return -EINVAL;
	}

	/* Verify firmware module length and checksum */
	cksum = checksum((u8*)&cfm->info, sizeof(cfm_info_t) +
					      cfm->info.codesize);
/*
        FIXME cfm->info.codesize is off by 2
	if (((len - sizeof(cfm_t) - 1) != cfm->info.codesize) ||
*/
	if (cksum != cfm->checksum) {
		printk(KERN_ERR "%s:cycx_load: firmware corrupted!\n", modname);
		printk(KERN_ERR " cdsize = 0x%x (expected 0x%lx)\n",
                       len - sizeof(cfm_t) - 1, cfm->info.codesize);
                printk(KERN_ERR " chksum = 0x%x (expected 0x%x)\n",
                       cksum, cfm->checksum);
		return -EINVAL;
	}

	/* If everything is ok, set reset, data and code pointers */

	img_hdr = (cycx_header_t*)(((u8*) cfm) + sizeof(cfm_t) - 1);
#ifdef FIRMWARE_DEBUG
	printk(KERN_INFO "%s:cycx_load: image sizes\n", modname);
	printk(KERN_INFO " reset=%lu\n", img_hdr->reset_size);
	printk(KERN_INFO "  data=%lu\n", img_hdr->data_size);
	printk(KERN_INFO "  code=%lu\n", img_hdr->code_size);
#endif
	reset_image = ((u8 *) img_hdr) + sizeof(cycx_header_t);
	data_image = reset_image + img_hdr->reset_size;
	code_image = data_image + img_hdr->data_size;

	/*---- Start load ----*/
        /* Announce */
	printk(KERN_INFO "%s: loading firmware %s (ID=%u)...\n", modname,
		(cfm->descr[0] != '\0') ? cfm->descr : "unknown firmware",
		cfm->info.codeid);

	for (i = 0 ; i < 5 ; i++) {
		/* Reset Cyclom hardware */
		if ((status = cycx_reset(hw)) != 0) {
			printk(KERN_ERR "%s: dpm problem or board not "
					"found (%d).\n", modname, status);
			return -EINVAL;
		}

		/* Load reset.bin */
                cycx_reset_boot(hw->dpmbase, reset_image, img_hdr->reset_size);
		/* reset is waiting for boot */
		writew(GEN_POWER_ON, pt_cycld);
		delay_cycx(1);

		for (j = 0 ; j < 3 ; j++)
			if (!readw(pt_cycld)) goto reset_loaded;
			else delay_cycx(1);
	}

	printk(KERN_ERR "%s: reset not started.\n", modname);
	return -EINVAL;
reset_loaded:
	/* Load data.bin */
	if((status = cycx_data_boot(hw->dpmbase, data_image, 
		                    img_hdr->data_size)) != 0) {
		printk(KERN_ERR "%s: cannot load data file (%d).\n",
				modname, status);
		return -EINVAL;
	}

	/* Load code.bin */
	if((status = cycx_code_boot(hw->dpmbase, code_image, 
		                    img_hdr->code_size)) != 0) {
		printk(KERN_ERR "%s: cannot load code file (%d).\n",
				modname, status);
		return -EINVAL;
	}

	/* Prepare boot-time configuration data */
	cycx_bootcfg(hw);

	/* kick-off CPU */
	cycx_start(hw->dpmbase);

	/* Arthur Ganzert's tip: wait a while after the firmware loading...
	   seg abr 26 17:17:12 EST 1999 - acme */
	delay_cycx(7);
	printk(KERN_INFO "%s: firmware loaded!\n", modname);

	/* enable interrupts */
        if (cycx_inten(hw)) {
		printk(KERN_ERR "%s: adapter hardware failure!\n", modname);
		return -EIO;
	}

	return 0;
}

/* Prepare boot-time firmware configuration data.
 * o initialize configuration data area
   From async.doc - V_3.4.0 - 07/18/1994
   - As of now, only static buffers are available to the user.
     So, the bit VD_RXDIRC must be set in 'valid'. That means that user
     wants to use the static transmission and reception buffers. */
static void cycx_bootcfg (cycxhw_t *hw)
{
	/* use fixed buffers */
	writeb(FIXED_BUFFERS, hw->dpmbase + CONF_OFFSET); 
}

/* Initialize CYCX_2X adapter. */
static int init_cycx_2x (cycxhw_t *hw)
{
	if (!detect_cycx_2x(hw->dpmbase)) return -ENODEV;
	return 0;
}

/* Detect Cyclom 2x adapter.
 *	Following tests are used to detect Cyclom 2x adapter:
 *       to be completed based on the tests done below
 *	Return 1 if detected o.k. or 0 if failed.
 *	Note:	This test is destructive! Adapter will be left in shutdown
 *		state after the test. */
static int detect_cycx_2x (u32 addr)
{
	printk(KERN_INFO "%s: looking for a cyclom 2x at 0x%lX...\n",
			 modname, (unsigned long) addr);

	reset_cycx_2x(addr);
	return memory_exists(addr);
}

/* Miscellaneous */
/* Get option's index into the options list.
 *	Return option's index (1 .. N) or zero if option is invalid. */
static int get_option_index (u32 *optlist, u32 optval)
{
	int i = 1;
	for (; i <= optlist[0]; ++i) if (optlist[i] == optval) return i;
	return 0;
}

/* Reset adapter's CPU. */
static int reset_cycx_2x (u32 addr)
{
	writeb (0, addr + RST_ENABLE);  delay_cycx (2);
	writeb (0, addr + RST_DISABLE); delay_cycx (2);
	return memory_exists(addr) ? 0 : 1;
}

/* Delay */
static void delay_cycx (int sec)
{
/* acme
   Thu dez 31 21:45:16 EDT 1998
   FIXME I'll keep this comment here just in case, as of now I don't
   know it all the contexts where this routine is used are interruptible... */

	current->state = TASK_INTERRUPTIBLE;
	current->counter = 0;       /* make us low-priority */
	schedule_timeout(sec*HZ);
}

/* Calculate 16-bit CRC using CCITT polynomial. */
static u16 checksum (u8 *buf, u32 len)
{
	u16 crc = 0;
	u16 mask, flag;

	for (; len; --len, ++buf)
		for (mask = 0x80; mask; mask >>= 1) {
			flag = (crc & 0x8000);
			crc <<= 1;
			crc |= ((*buf & mask) ? 1 : 0);
			if (flag) crc ^= 0x1021;
		}

	return crc;
}
/* End */
