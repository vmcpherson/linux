/*
 * ILI9325 Framebuffer
 *
 * The ili9325 is configured in 16 bit mode and attached to
 * raspberry PI through an 8 bit data bus with a 8 bit latch used to
 * create a 16 bit bus. The databus is write only.
 * 
 * Code is largely the work Jean-Christian de Rivaz (as below) with modification
 * to use the ili9325 in 16 bit mode through a 8 bit latch.
 *
 *******************************************************************************
 * 
 * ToDo: Fix this text vv
 *
 * Original: Copyright (c) 2009 Jean-Christian de Rivaz
 *
 * Console support, 320x240 instead of 240x320:
 * Copyright (c) 2012 Jeroen Domburg <jeroen@spritesmods.com>
 *
 * Bits and pieces borrowed from the fsl-ili9325.c:
 * Copyright (C) 2010-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 * Author: Alison Wang <b18965@freescale.com>
 *         Jason Jin <Jason.jin@freescale.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * The Solomon Systech ILI9325 chip drive TFT screen up to 240x320. 
 *
 * For direct I/O-mode:
 *
 * This driver expect the SSD1286 to be connected to a 16 bits local bus
 * and to be set in the 16 bits parallel interface mode. To use it you must
 * define in your board file a struct platform_device with a name set to
 * "ili9325" and a struct resource array with two IORESOURCE_MEM: the first
 * for the control register; the second for the data register.
 *
 *
 * LCDs in their own, native SPI mode aren't supported yet, mostly because I 
 * can't get my hands on a cheap one.
 */

//#define DEBUG

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>
#include <asm/io.h>
#include <linux/delay.h>

#include <mach/platform.h>


#define LCD_NCS     4		// 7    // GPIO 4
#define LCD_NWR		17      // 0    // GPIO 17
#define LCD_RS		18      // 1    // GPIO 18

#define LCD_NRST	7      // 11    // GPIO 7

#define LCD_D10		22     // 3     // GPIO 22 *
#define LCD_D11		23     // 4     // GPIO 23 *
#define LCD_D12		24     // 5     // GPIO 24 *
#define LCD_D13		10     // 12    // GPIO 10 *
#define LCD_D14		25     // 6     // GPIO 25 *
#define LCD_D15		9      // 13	// GPIO 9  *
#define LCD_D16		11     // 14	// GPIO 11 *
#define LCD_D17		8      // 10	// GPIO 8

#define LCD_LE		27     // 2     // GPIO 27

#define LCD_D1		22     // 3     // GPIO 22   PD10
#define LCD_D2		10     // 12	// GPIO 10   PD13
#define LCD_D3		23     // 4     // GPIO 23   PD11
#define LCD_D4		25     // 6     // GPIO 25   PD14
#define LCD_D5		24     // 5     // GPIO 24   PD12
#define LCD_D6		11     // 14	// GPIO 11   PD16
#define LCD_D7		9      // 13	// GPIO 9    PD15
#define LCD_D8		8      // 10	// GPIO 8    PD10

// #define UPSIDEDOWN

struct ili9325_page {
	unsigned short x;
	unsigned short y;
	unsigned short *buffer;
	unsigned short *oldbuffer;
	unsigned short len;
	int must_update;
};

struct ili9325 {
	struct device *dev;
	struct fb_info *info;
	unsigned int pages_count;
	struct ili9325_page *pages;
	unsigned long pseudo_palette[17];
	int backlight;
};

/*
Use direct GPIO reg access instead of the gpiolib framework: because we want to write
multiple bits at once to the (presumably connected over a slower bus) GPIO block,
this involves less writes and so will be faster.
*/


/* macros to get at IO space when running virtually */
#define GPIOSET(no, ishigh)	{ if (ishigh) set|=(1<<no); else reset|=(1<<no); } while(0)


static inline void ili9325_reset(void) {
	writel((1<<LCD_NRST),  __io_address(GPIO_BASE+0x28));
	msleep(200);
	writel((1<<LCD_NWR),  __io_address(GPIO_BASE+0x1C));
	msleep(200);
}

#define GPIO_ALT_OFFSET(g) ((((g)/10))*4)
#define GPIO_ALT_VAL(a, g) ((a)<<(((g)%10)*3))
static inline void ili9325_set_output(int gpio) 
{
	unsigned int v;
	v=readl(__io_address(GPIO_BASE+GPIO_ALT_OFFSET(gpio)));
	v&=~GPIO_ALT_VAL(0x7, gpio); //clear existing bits
	v|=GPIO_ALT_VAL(1, gpio); //output
	writel(v, __io_address(GPIO_BASE+GPIO_ALT_OFFSET(gpio)));
}



static inline void ili9325_writeword(unsigned int data, int rs)
{
	unsigned int set=0;
	unsigned int reset=0;

	GPIOSET(LCD_NCS, 0);
	GPIOSET(LCD_NWR, 1);
	GPIOSET(LCD_RS, rs);
	GPIOSET(LCD_NRST, 1);
	GPIOSET(LCD_LE, 0);
	
	// Latch LSB
	//
	GPIOSET(LCD_D1, (data&0x01));
	GPIOSET(LCD_D2, (data&0x02));
	GPIOSET(LCD_D3, (data&0x04));
	GPIOSET(LCD_D4, (data&0x08));
	GPIOSET(LCD_D5, (data&0x10));
	GPIOSET(LCD_D6, (data&0x20));
	GPIOSET(LCD_D7, (data&0x40));
	GPIOSET(LCD_D8, (data&0x80));

	writel(set, __io_address(GPIO_BASE+0x1C));
	writel(reset, __io_address(GPIO_BASE+0x28));

    // Latch value
	writel((1 << LCD_LE),  __io_address(GPIO_BASE+0x1C));    
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
#if 0
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
#endif
	writel((1 << LCD_LE),  __io_address(GPIO_BASE+0x28));
    
	// Write MSB
	//
    set = 0;
    reset = 0;
	GPIOSET(LCD_D10, (data&0x0100));
	GPIOSET(LCD_D11, (data&0x0200));
	GPIOSET(LCD_D12, (data&0x0400));
	GPIOSET(LCD_D13, (data&0x0800));
	GPIOSET(LCD_D14, (data&0x1000));
	GPIOSET(LCD_D15, (data&0x2000));
	GPIOSET(LCD_D16, (data&0x4000));
	GPIOSET(LCD_D17, (data&0x8000));

	writel(set, __io_address(GPIO_BASE+0x1C));
	writel(reset, __io_address(GPIO_BASE+0x28));

	//Pulse /wr low
    //
	writel((1<<LCD_NWR),  __io_address(GPIO_BASE+0x28));
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
#if 0
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
	writel(0,  __io_address(GPIO_BASE+0x28)); //used as a delay
#endif
	writel((1<<LCD_NWR),  __io_address(GPIO_BASE+0x1C));
}

static void ili9325_setptr(struct ili9325 *item, int x, int y) {
#ifdef UPSIDEDOWN
	ili9325_writeword(0x0020, 0); ili9325_writeword(y, 1); // Horizontal GRAM Start Address
	ili9325_writeword(0x0021, 0); ili9325_writeword((item->info->var.xres - 1)-x, 1); // Vertical GRAM Start Address
#else
	ili9325_writeword(0x0020, 0); ili9325_writeword((item->info->var.yres - 1)-y, 1); // Horizontal GRAM Start Address
	ili9325_writeword(0x0021, 0); ili9325_writeword(x, 1); // Vertical GRAM Start Address
#endif
	ili9325_writeword(0x0022, 0);
}


static inline void ili9325_gpio_init(void) 
{
	ili9325_set_output(LCD_NCS);
	ili9325_set_output(LCD_NWR);
	ili9325_set_output(LCD_RS);
	ili9325_set_output(LCD_NRST);
	ili9325_set_output(LCD_D10);
	ili9325_set_output(LCD_D11);
	ili9325_set_output(LCD_D12);
	ili9325_set_output(LCD_D13);
	ili9325_set_output(LCD_D14);
	ili9325_set_output(LCD_D15);
	ili9325_set_output(LCD_D16);
	ili9325_set_output(LCD_D17);

	ili9325_set_output(LCD_LE); // VRM
    
	ili9325_writeword(0,0);
}


static void ili9325_copy(struct ili9325 *item, unsigned int index)
{
	unsigned short x;
	unsigned short y;
	unsigned short *buffer;
	unsigned short *oldbuffer;
	unsigned int len;
	unsigned int count;
	int sendNewPos=1;
	x = item->pages[index].x;
	y = item->pages[index].y;
	buffer = item->pages[index].buffer;
	oldbuffer = item->pages[index].oldbuffer;
	len = item->pages[index].len;
	dev_dbg(item->dev,
		"%s: page[%u]: x=%3hu y=%3hu buffer=0x%p len=%3hu\n",
		__func__, index, x, y, buffer, len);

	//Only update changed pixels in the page.
	for (count = 0; count < len; count++) {
		if (buffer[count]==oldbuffer[count]) {
			sendNewPos=1;
		} else {
			if (sendNewPos) {
				ili9325_setptr(item, x, y);
				sendNewPos=0;
			}
			ili9325_writeword(buffer[count], 1);
			oldbuffer[count]=buffer[count];
		}
		x++;
		if (x>=item->info->var.xres) {
			y++;
			x=0;
		}
	}
}

static void ili9325_update_all(struct ili9325 *item)
{
	unsigned short i;
	struct fb_deferred_io *fbdefio = item->info->fbdefio;
	for (i = 0; i < item->pages_count; i++) {
		item->pages[i].must_update=1;
	}
	schedule_delayed_work(&item->info->deferred_work, fbdefio->delay);
}

static void ili9325_update(struct fb_info *info, struct list_head *pagelist)
{
	struct ili9325 *item = (struct ili9325 *)info->par;
	struct page *page;
	int i;

	//We can be called because of pagefaults (mmap'ed framebuffer, pages
	//returned in *pagelist) or because of kernel activity 
	//(pages[i]/must_update!=0). Add the former to the list of the latter.
	list_for_each_entry(page, pagelist, lru) {
		item->pages[page->index].must_update=1;
	}

	//Copy changed pages.
	for (i=0; i<item->pages_count; i++) {
		//ToDo: Small race here between checking and setting must_update, 
		//maybe lock?
		if (item->pages[i].must_update) {
			item->pages[i].must_update=0;
			ili9325_copy(item, i);
		}
	}

}


static void __init ili9325_setup(struct ili9325 *item)
{
	/*
	WARNING: This init-sequence is partially derived from example code, partially
	from the datasheet and partially (especially the msleep()s) from experimentation.
	This seems to work best on the LCDs I have; YMMV.
	*/
	int x;
	dev_dbg(item->dev, "%s: item=0x%p\n", __func__, (void *)item);
	ili9325_gpio_init();
	ili9325_reset();
	ili9325_writeword(0x00E3, 0); ili9325_writeword(0x3008, 1); // Set internal timing
	ili9325_writeword(0x00E7, 0); ili9325_writeword(0x0012, 1); // Set internal timing
	ili9325_writeword(0x00EF, 0); ili9325_writeword(0x1231, 1); // Set internal timing
	msleep(200);
	ili9325_writeword(0x0001, 0); ili9325_writeword(0x0100, 1); // set SS and SM bit
	msleep(200);
	ili9325_writeword(0x0002, 0); ili9325_writeword(0x0700, 1); // set 1 line inversion
#ifdef UPSIDEDOWN
	ili9325_writeword(0x0003, 0); ili9325_writeword(0x1018, 1); // set GRAM write direction and BGR=1.
#else
	ili9325_writeword(0x0003, 0); ili9325_writeword(0x1028, 1); // set GRAM write direction and BGR=1.
#endif
	msleep(100);
	ili9325_writeword(0x0004, 0); ili9325_writeword(0x0000, 1); // Resize register
	msleep(100);
	ili9325_writeword(0x0008, 0); ili9325_writeword(0x0207, 1); // set the back porch and front porch
	msleep(100);
	ili9325_writeword(0x0009, 0); ili9325_writeword(0x0000, 1); // set non-display area refresh cycle ISC[3:0]
	msleep(100);
	ili9325_writeword(0x000A, 0); ili9325_writeword(0x0000, 1); // FMARK function
	msleep(100);
	ili9325_writeword(0x000C, 0); ili9325_writeword(0x0000, 1); // RGB interface setting
	msleep(100);
	ili9325_writeword(0x000D, 0); ili9325_writeword(0x0000, 1); // Frame marker Position
	msleep(100);
	ili9325_writeword(0x000F, 0); ili9325_writeword(0x0000, 1); // RGB interface polarity
	//--------------Power On sequence --------------//
	msleep(100); //'wait 2 frames or more'
	ili9325_writeword(0x0010, 0); ili9325_writeword(0x0000, 1); // SAP); Trans_Dat_16(BT[3:0]); Trans_Dat_16(AP); Trans_Dat_16(DSTB); Trans_Dat_16(SLP); Trans_Dat_16(STB
	msleep(100); //'wait 2 frames or more'
	ili9325_writeword(0x0011, 0); ili9325_writeword(0x0007, 1); // DC1[2:0]); Trans_Dat_16(DC0[2:0]); Trans_Dat_16(VC[2:0]
	msleep(100); //'wait 2 frames or more'
	ili9325_writeword(0x0012, 0); ili9325_writeword(0x0000, 1); // VREG1OUT voltage
	msleep(100);
	ili9325_writeword(0x0013, 0); ili9325_writeword(0x0000, 1); // VDV[4:0] for VCOM amplitude
	msleep(400); // Dis-charge capacitor power voltage
	ili9325_writeword(0x0010, 0); ili9325_writeword(0x1490, 1); // SAP); Trans_Dat_16(BT[3:0]); Trans_Dat_16(AP); Trans_Dat_16(DSTB); Trans_Dat_16(SLP); Trans_Dat_16(STB
	ili9325_writeword(0x0011, 0); ili9325_writeword(0x0227, 1); // R11h=0x0221 at VCI=3.3V); Trans_Dat_16(DC1[2:0]); Trans_Dat_16(DC0[2:0]); Trans_Dat_16(VC[2:0]
	msleep(100); // Delayms 50m
	ili9325_writeword(0x0012, 0); ili9325_writeword(0x001c, 1); // External reference voltage= Vci;
	msleep(100); // Delayms 50ms
	ili9325_writeword(0x0013, 0); ili9325_writeword(0x0A00, 1); // R13=0F00 when R12=009E;VDV[4:0] for VCOM amplitude
	ili9325_writeword(0x0029, 0); ili9325_writeword(0x000F, 1); // R29=0019 when R12=009E;VCM[5:0] for VCOMH//0012//
	ili9325_writeword(0x002B, 0); ili9325_writeword(0x000D, 1); // Frame Rate = 91Hz
	msleep(100); // Delayms 50ms
	ili9325_writeword(0x0020, 0); ili9325_writeword(0x0000, 1); // GRAM horizontal Address
	ili9325_writeword(0x0021, 0); ili9325_writeword(0x0000, 1); // GRAM Vertical Address
	// ----------- Adjust the Gamma Curve ----------//

#if 0
	ili9325_writeword(0x0030, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0031, 0); ili9325_writeword(0x0203, 1);
	ili9325_writeword(0x0032, 0); ili9325_writeword(0x0001, 1);
	ili9325_writeword(0x0035, 0); ili9325_writeword(0x0205, 1);
	ili9325_writeword(0x0036, 0); ili9325_writeword(0x030C, 1);
	ili9325_writeword(0x0037, 0); ili9325_writeword(0x0607, 1);
	ili9325_writeword(0x0038, 0); ili9325_writeword(0x0405, 1);
	ili9325_writeword(0x0039, 0); ili9325_writeword(0x0707, 1);
	ili9325_writeword(0x003C, 0); ili9325_writeword(0x0502, 1);
	ili9325_writeword(0x003D, 0); ili9325_writeword(0x1008, 1);
#else


	ili9325_writeword(0x0030, 0); ili9325_writeword(0x0006, 1);
	ili9325_writeword(0x0031, 0); ili9325_writeword(0x0101, 1);
	ili9325_writeword(0x0032, 0); ili9325_writeword(0x0003, 1);
    
	ili9325_writeword(0x0035, 0); ili9325_writeword(0x0106, 1);
	ili9325_writeword(0x0036, 0); ili9325_writeword(0x0b02, 1);
	ili9325_writeword(0x0037, 0); ili9325_writeword(0x0302, 1);
	ili9325_writeword(0x0038, 0); ili9325_writeword(0x0707, 1);
	ili9325_writeword(0x0039, 0); ili9325_writeword(0x0007, 1);
    
	ili9325_writeword(0x003C, 0); ili9325_writeword(0x0600, 1);
	ili9325_writeword(0x003D, 0); ili9325_writeword(0x020b, 1);
    
#endif
    
	//------------------ Set GRAM area ---------------//
	msleep(100);
	ili9325_writeword(0x0050, 0); ili9325_writeword(0x0000, 1); // Horizontal GRAM Start Address
	ili9325_writeword(0x0051, 0); ili9325_writeword(0x00EF, 1); // Horizontal GRAM End Address
	ili9325_writeword(0x0052, 0); ili9325_writeword(0x0000, 1); // Vertical GRAM Start Address
	ili9325_writeword(0x0053, 0); ili9325_writeword(0x013F, 1); // Vertical GRAM Start Address
	ili9325_writeword(0x0060, 0); ili9325_writeword(0xA700, 1); // Gate Scan Line
	ili9325_writeword(0x0061, 0); ili9325_writeword(0x0001, 1); // NDL,VLE); Trans_Dat_16(REV
	ili9325_writeword(0x006A, 0); ili9325_writeword(0x0000, 1); // set scrolling line
	msleep(500);
	//-------------- Partial Display Control ---------//
	ili9325_writeword(0x0080, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0081, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0082, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0083, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0084, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0085, 0); ili9325_writeword(0x0000, 1);
	msleep(100);
	//-------------- Panel Control -------------------//
	ili9325_writeword(0x0090, 0); ili9325_writeword(0x0010, 1);
	ili9325_writeword(0x0092, 0); ili9325_writeword(0x0600, 1);//0x0000
	ili9325_writeword(0x0093, 0); ili9325_writeword(0x0003, 1);
	ili9325_writeword(0x0095, 0); ili9325_writeword(0x0110, 1);
	ili9325_writeword(0x0097, 0); ili9325_writeword(0x0000, 1);
	ili9325_writeword(0x0098, 0); ili9325_writeword(0x0000, 1);
	msleep(100); // Delayms 50ms
	ili9325_writeword(0x0007, 0); ili9325_writeword(0x0133, 1); // 262K color and display ON
	msleep(200);
	ili9325_writeword(0x0022, 0);
	ili9325_writeword(0x0022, 0);
	//Clear screen
	ili9325_setptr(item, 0, 0);
	for (x=0; x<320*240; x++) ili9325_writeword(0, 1);
	ili9325_setptr(item, 0, 0);
	for (x=0; x<320*240; x++) ili9325_writeword(item->pages[0].buffer[x], 1);
}

//This routine will allocate the buffer for the complete framebuffer. This
//is one continuous chunk of 16-bit pixel values; userspace programs
//will write here.
static int __init ili9325_video_alloc(struct ili9325 *item)
{
	unsigned int frame_size;

	dev_dbg(item->dev, "%s: item=0x%p\n", __func__, (void *)item);

	frame_size = item->info->fix.line_length * item->info->var.yres;
	dev_dbg(item->dev, "%s: item=0x%p frame_size=%u\n",
		__func__, (void *)item, frame_size);

	item->pages_count = frame_size / PAGE_SIZE;
	if ((item->pages_count * PAGE_SIZE) < frame_size) {
		item->pages_count++;
	}
	dev_dbg(item->dev, "%s: item=0x%p pages_count=%u\n",
		__func__, (void *)item, item->pages_count);

	item->info->fix.smem_len = item->pages_count * PAGE_SIZE;
	item->info->fix.smem_start =
	    (unsigned long)vmalloc(item->info->fix.smem_len);
	if (!item->info->fix.smem_start) {
		dev_err(item->dev, "%s: unable to vmalloc\n", __func__);
		return -ENOMEM;
	}
	memset((void *)item->info->fix.smem_start, 0, item->info->fix.smem_len);

	return 0;
}

static void ili9325_video_free(struct ili9325 *item)
{
	dev_dbg(item->dev, "%s: item=0x%p\n", __func__, (void *)item);

	kfree((void *)item->info->fix.smem_start);
}

//This routine will allocate a ili9325_page struct for each vm page in the
//main framebuffer memory. Each struct will contain a pointer to the page
//start, an x- and y-offset, and the length of the pagebuffer which is in the framebuffer.
static int __init ili9325_pages_alloc(struct ili9325 *item)
{
	unsigned short pixels_per_page;
	unsigned short yoffset_per_page;
	unsigned short xoffset_per_page;
	unsigned int index;
	unsigned short x = 0;
	unsigned short y = 0;
	unsigned short *buffer;
	unsigned short *oldbuffer;
	unsigned int len;

	dev_dbg(item->dev, "%s: item=0x%p\n", __func__, (void *)item);

	item->pages = kmalloc(item->pages_count * sizeof(struct ili9325_page),
			      GFP_KERNEL);
	if (!item->pages) {
		dev_err(item->dev, "%s: unable to kmalloc for ili9325_page\n",
			__func__);
		return -ENOMEM;
	}

	pixels_per_page = PAGE_SIZE / (item->info->var.bits_per_pixel / 8);
	yoffset_per_page = pixels_per_page / item->info->var.xres;
	xoffset_per_page = pixels_per_page -
	    (yoffset_per_page * item->info->var.xres);
	dev_dbg(item->dev, "%s: item=0x%p pixels_per_page=%hu "
		"yoffset_per_page=%hu xoffset_per_page=%hu\n",
		__func__, (void *)item, pixels_per_page,
		yoffset_per_page, xoffset_per_page);

	oldbuffer = kmalloc(item->pages_count * pixels_per_page * 2,
			      GFP_KERNEL);
	if (!oldbuffer) {
		dev_err(item->dev, "%s: unable to kmalloc for ili9325_page oldbuffer\n",
			__func__);
		return -ENOMEM;
	}

	buffer = (unsigned short *)item->info->fix.smem_start;
	for (index = 0; index < item->pages_count; index++) {
		len = (item->info->var.xres * item->info->var.yres) -
		    (index * pixels_per_page);
		if (len > pixels_per_page) {
			len = pixels_per_page;
		}
		dev_dbg(item->dev,
			"%s: page[%d]: x=%3hu y=%3hu buffer=0x%p len=%3hu\n",
			__func__, index, x, y, buffer, len);
		item->pages[index].x = x;
		item->pages[index].y = y;
		item->pages[index].buffer = buffer;
		item->pages[index].oldbuffer = oldbuffer;
		item->pages[index].len = len;

		x += xoffset_per_page;
		if (x >= item->info->var.xres) {
			y++;
			x -= item->info->var.xres;
		}
		y += yoffset_per_page;
		buffer += pixels_per_page;
		oldbuffer += pixels_per_page;
	}

	return 0;
}

static void ili9325_pages_free(struct ili9325 *item)
{
	dev_dbg(item->dev, "%s: item=0x%p\n", __func__, (void *)item);

	kfree(item->pages);
}

static inline __u32 CNVT_TOHW(__u32 val, __u32 width)
{
	return ((val<<width) + 0x7FFF - val)>>16;
}

//This routine is needed because the console driver won't work without it.
static int ili9325_setcolreg(unsigned regno,
			       unsigned red, unsigned green, unsigned blue,
			       unsigned transp, struct fb_info *info)
{
	int ret = 1;

	/*
	 * If greyscale is true, then we convert the RGB value
	 * to greyscale no matter what visual we are using.
	 */
	if (info->var.grayscale)
		red = green = blue = (19595 * red + 38470 * green +
				      7471 * blue) >> 16;
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		if (regno < 16) {
			u32 *pal = info->pseudo_palette;
			u32 value;

			red = CNVT_TOHW(red, info->var.red.length);
			green = CNVT_TOHW(green, info->var.green.length);
			blue = CNVT_TOHW(blue, info->var.blue.length);
			transp = CNVT_TOHW(transp, info->var.transp.length);

			value = (red << info->var.red.offset) |
				(green << info->var.green.offset) |
				(blue << info->var.blue.offset) |
				(transp << info->var.transp.offset);

			pal[regno] = value;
			ret = 0;
		}
		break;
	case FB_VISUAL_STATIC_PSEUDOCOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		break;
	}
	return ret;
}

static int ili9325_blank(int blank_mode, struct fb_info *info)
{
	struct ili9325 *item = (struct ili9325 *)info->par;
	if (blank_mode == FB_BLANK_UNBLANK)
		item->backlight=1;
	else
		item->backlight=0;
	//Item->backlight won't take effect until the LCD is written to. Force that
	//by dirty'ing a page.
	item->pages[0].must_update=1;
	schedule_delayed_work(&info->deferred_work, 0);
	return 0;
}

static void ili9325_touch(struct fb_info *info, int x, int y, int w, int h) 
{
	struct fb_deferred_io *fbdefio = info->fbdefio;
	struct ili9325 *item = (struct ili9325 *)info->par;
	int i, ystart, yend;
	if (fbdefio) {
		//Touch the pages the y-range hits, so the deferred io will update them.
		for (i=0; i<item->pages_count; i++) {
			ystart=item->pages[i].y;
			yend=item->pages[i].y+(item->pages[i].len/info->fix.line_length)+1;
			if (!((y+h)<ystart || y>yend)) {
				item->pages[i].must_update=1;
			}
		}
		//Schedule the deferred IO to kick in after a delay.
		schedule_delayed_work(&info->deferred_work, fbdefio->delay);
	}
}

static void ili9325_fillrect(struct fb_info *p, const struct fb_fillrect *rect) 
{
	sys_fillrect(p, rect);
	ili9325_touch(p, rect->dx, rect->dy, rect->width, rect->height);
}

static void ili9325_imageblit(struct fb_info *p, const struct fb_image *image) 
{
	sys_imageblit(p, image);
	ili9325_touch(p, image->dx, image->dy, image->width, image->height);
}

static void ili9325_copyarea(struct fb_info *p, const struct fb_copyarea *area) 
{
	sys_copyarea(p, area);
	ili9325_touch(p, area->dx, area->dy, area->width, area->height);
}

static ssize_t ili9325_write(struct fb_info *p, const char __user *buf, 
				size_t count, loff_t *ppos) 
{
	ssize_t res;
	res = fb_sys_write(p, buf, count, ppos);
	ili9325_touch(p, 0, 0, p->var.xres, p->var.yres);
	return res;
}

static struct fb_ops ili9325_fbops = {
	.owner        = THIS_MODULE,
	.fb_read      = fb_sys_read,
	.fb_write     = ili9325_write,
	.fb_fillrect  = ili9325_fillrect,
	.fb_copyarea  = ili9325_copyarea,
	.fb_imageblit = ili9325_imageblit,
	.fb_setcolreg	= ili9325_setcolreg,
	.fb_blank	= ili9325_blank,
};

static struct fb_fix_screeninfo ili9325_fix __initdata = {
	.id          = "ILI9325",
	.type        = FB_TYPE_PACKED_PIXELS,
	.visual      = FB_VISUAL_TRUECOLOR,
	.accel       = FB_ACCEL_NONE,
	.line_length = 320 * 2,
};

static struct fb_var_screeninfo ili9325_var __initdata = {
	.xres		= 320,
	.yres		= 240,
	.xres_virtual	= 320,
	.yres_virtual	= 240,
	.width		= 320,
	.height		= 240,
	.bits_per_pixel	= 16,
	.red		= {11, 5, 0},
	.green		= {5, 6, 0},
	.blue		= {0, 5, 0},
	.activate	= FB_ACTIVATE_NOW,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_deferred_io ili9325_defio = {
        .delay          = HZ / 25,
        .deferred_io    = &ili9325_update,
};

static int __init ili9325_probe(struct platform_device *dev)
{
	int ret = 0;
	struct ili9325 *item;
	struct fb_info *info;

	dev_dbg(&dev->dev, "%s\n", __func__);

	item = kzalloc(sizeof(struct ili9325), GFP_KERNEL);
	if (!item) {
		dev_err(&dev->dev,
			"%s: unable to kzalloc for ili9325\n", __func__);
		ret = -ENOMEM;
		goto out;
	}
	item->dev = &dev->dev;
	dev_set_drvdata(&dev->dev, item);
	item->backlight=1;


	info = framebuffer_alloc(sizeof(struct ili9325), &dev->dev);
	if (!info) {
		ret = -ENOMEM;
		dev_err(&dev->dev,
			"%s: unable to framebuffer_alloc\n", __func__);
		goto out_item;
	}
	info->pseudo_palette = &item->pseudo_palette;
	item->info = info;
	info->par = item;
	info->dev = &dev->dev;
	info->fbops = &ili9325_fbops;
	info->flags = FBINFO_FLAG_DEFAULT|FBINFO_VIRTFB;
	info->fix = ili9325_fix;
	info->var = ili9325_var;

	ret = ili9325_video_alloc(item);
	if (ret) {
		dev_err(&dev->dev,
			"%s: unable to ili9325_video_alloc\n", __func__);
		goto out_info;
	}
	info->screen_base = (char __iomem *)item->info->fix.smem_start;

	ret = ili9325_pages_alloc(item);
	if (ret < 0) {
		dev_err(&dev->dev,
			"%s: unable to ili9325_pages_init\n", __func__);
		goto out_video;
	}

	info->fbdefio = &ili9325_defio;
	fb_deferred_io_init(info);

	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&dev->dev,
			"%s: unable to register_frambuffer\n", __func__);
		goto out_pages;
	}


	ili9325_setup(item);
	ili9325_update_all(item);

	return ret;

out_pages:
	ili9325_pages_free(item);
out_video:
	ili9325_video_free(item);
out_info:
	framebuffer_release(info);
out_item:
	kfree(item);
out:
	return ret;
}


static int ili9325_remove(struct platform_device *dev)
{
	struct fb_info *info = dev_get_drvdata(&dev->dev);
	struct ili9325 *item = (struct ili9325 *)info->par;
	unregister_framebuffer(info);
	ili9325_pages_free(item);
	ili9325_video_free(item);
	framebuffer_release(info);
	kfree(item);
	return 0;
}

#ifdef CONFIG_PM
static int ili9325_suspend(struct platform_device *dev, pm_message_t state)
{
//	struct fb_info *info = dev_get_drvdata(&spi->dev);
//	struct ili9325 *item = (struct ili9325 *)info->par;
	/* enter into sleep mode */
//	ili9325_reg_set(item, ILI9325_REG_SLEEP_MODE, 0x0001);
	return 0;
}

static int ili9325_resume(struct platform_device *dev)
{
//	struct fb_info *info = dev_get_drvdata(&spi->dev);
//	struct ili9325 *item = (struct ili9325 *)info->par;
	/* leave sleep mode */
//	ili9325_reg_set(item, ILI9325_REG_SLEEP_MODE, 0x0000);
	return 0;
}
#else
#define ili9325_suspend NULL
#define ili9325_resume NULL
#endif

static struct platform_driver ili9325_driver = {
	.probe = ili9325_probe,
	.driver = {
		   .name = "ili9325",
		   },
};

static int __init ili9325_init(void)
{
	int ret = 0;

	pr_debug("%s\n", __func__);

	ret = platform_driver_register(&ili9325_driver);
	if (ret) {
		pr_err("%s: unable to platform_driver_register\n", __func__);
	}

	return ret;
}

module_init(ili9325_init);
module_exit(ili9325_remove);


MODULE_DESCRIPTION("ILI9325 LCD Driver for VRM");
MODULE_AUTHOR("Vaughan McPherson <vmcpherson@gmail.com>");
MODULE_LICENSE("GPL");
