/*
 * (C) Copyright Eric Anholt 2006
 * (C) Copyright IBM Corporation 2006
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * IBM AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file freebsd_pci.c
 *
 * Access the kernel PCI support using /dev/pci's ioctl and mmap interface.
 *
 * \author Eric Anholt <eric@anholt.net>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/pciio.h>
#include <sys/mman.h>
#include <sys/memrange.h>

#include "config.h"
#include "pciaccess.h"
#include "pciaccess_private.h"

#define	PCIC_DISPLAY	0x03
#define	PCIS_DISPLAY_VGA	0x00
#define	PCIS_DISPLAY_XGA	0x01
#define	PCIS_DISPLAY_3D		0x02
#define	PCIS_DISPLAY_OTHER	0x80

/* Registers taken from pcireg.h */
#define PCIR_COMMAND    0x04
#define PCIM_CMD_PORTEN         0x0001
#define PCIM_CMD_MEMEN          0x0002
#define PCIR_BIOS	0x30
#define PCIM_BIOS_ENABLE	0x01
#define PCIM_BIOS_ADDR_MASK	0xfffff800

#define PCIR_BARS       0x10
#define PCIR_BAR(x)             (PCIR_BARS + (x) * 4)
#define PCI_BAR_IO(x)           (((x) & PCIM_BAR_SPACE) == PCIM_BAR_IO_SPACE)
#define PCI_BAR_MEM(x)          (((x) & PCIM_BAR_SPACE) == PCIM_BAR_MEM_SPACE)
#define PCIM_BAR_MEM_TYPE       0x00000006
#define PCIM_BAR_MEM_64         4
#define PCIM_BAR_MEM_PREFETCH   0x00000008
#define PCIM_BAR_SPACE          0x00000001
#define PCIM_BAR_MEM_SPACE      0
#define PCIM_BAR_IO_SPACE       1

/**
 * FreeBSD private pci_system structure that extends the base pci_system
 * structure.
 *
 * It is initialized once and used as a global, just as pci_system is used.
 */
_pci_hidden
struct freebsd_pci_system {
    /* This must be the first entry in the structure, as pci_system_cleanup()
     * frees pci_sys.
     */
    struct pci_system pci_sys;

    int pcidev; /**< fd for /dev/pci */
} *freebsd_pci_sys;

/**
 * Map a memory region for a device using /dev/mem.
 *
 * \param dev   Device whose memory region is to be mapped.
 * \param map   Parameters of the mapping that is to be created.
 *
 * \return
 * Zero on success or an \c errno value on failure.
 */
static int
pci_device_freebsd_map_range(struct pci_device *dev,
			     struct pci_device_mapping *map)
{
    const int prot = ((map->flags & PCI_DEV_MAP_FLAG_WRITABLE) != 0)
        ? (PROT_READ | PROT_WRITE) : PROT_READ;
    struct mem_range_desc mrd;
    struct mem_range_op mro;

    int fd, err = 0;

    fd = open("/dev/mem", O_RDWR);
    if (fd == -1)
	return errno;

    map->memory = mmap(NULL, map->size, prot, MAP_SHARED, fd, map->base);

    if (map->memory == MAP_FAILED) {
	err = errno;
    }

    mrd.mr_base = map->base;
    mrd.mr_len = map->size;
    strncpy(mrd.mr_owner, "pciaccess", sizeof(mrd.mr_owner));
    if (map->flags & PCI_DEV_MAP_FLAG_CACHABLE)
	mrd.mr_flags = MDF_WRITEBACK;
    else if (map->flags & PCI_DEV_MAP_FLAG_WRITE_COMBINE)
	mrd.mr_flags = MDF_WRITECOMBINE;
    else
	mrd.mr_flags = MDF_UNCACHEABLE;
    mro.mo_desc = &mrd;
    mro.mo_arg[0] = MEMRANGE_SET_UPDATE;

    /* No need to set an MTRR if it's the default mode. */
    if (mrd.mr_flags != MDF_UNCACHEABLE) {
	if (ioctl(fd, MEMRANGE_SET, &mro)) {
	    fprintf(stderr, "failed to set mtrr: %s\n", strerror(errno));
	}
    }

    close(fd);

    return err;
}

static int
pci_device_freebsd_unmap_range( struct pci_device *dev,
				struct pci_device_mapping *map )
{
    struct mem_range_desc mrd;
    struct mem_range_op mro;
    int fd;

    if ((map->flags & PCI_DEV_MAP_FLAG_CACHABLE) ||
	(map->flags & PCI_DEV_MAP_FLAG_WRITE_COMBINE))
    {
	fd = open("/dev/mem", O_RDWR);
	if (fd != -1) {
	    mrd.mr_base = map->base;
	    mrd.mr_len = map->size;
	    strncpy(mrd.mr_owner, "pciaccess", sizeof(mrd.mr_owner));
	    mrd.mr_flags = MDF_UNCACHEABLE;
	    mro.mo_desc = &mrd;
	    mro.mo_arg[0] = MEMRANGE_SET_REMOVE;

	    if (ioctl(fd, MEMRANGE_SET, &mro)) {
		fprintf(stderr, "failed to unset mtrr: %s\n", strerror(errno));
	    }

	    close(fd);
	} else {
	    fprintf(stderr, "Failed to open /dev/mem\n");
	}
    }

    return pci_device_generic_unmap_range(dev, map);
}

static int
pci_device_freebsd_read( struct pci_device * dev, void * data,
			 pciaddr_t offset, pciaddr_t size,
			 pciaddr_t * bytes_read )
{
    struct pci_io io;

#if HAVE_PCI_IO_PC_DOMAIN
    io.pi_sel.pc_domain = dev->domain;
#endif
    io.pi_sel.pc_bus = dev->bus;
    io.pi_sel.pc_dev = dev->dev;
    io.pi_sel.pc_func = dev->func;

    *bytes_read = 0;
    while ( size > 0 ) {
	int toread = (size < 4) ? size : 4;

	/* Only power of two allowed. */
	if (toread == 3)
	    toread = 2;

	io.pi_reg = offset;
	io.pi_width = toread;

	if ( ioctl( freebsd_pci_sys->pcidev, PCIOCREAD, &io ) < 0 )
	    return errno;

	memcpy(data, &io.pi_data, toread );

	offset += toread;
	data = (char *)data + toread;
	size -= toread;
	*bytes_read += toread;
    }

    return 0;
}


static int
pci_device_freebsd_write( struct pci_device * dev, const void * data,
			  pciaddr_t offset, pciaddr_t size,
			  pciaddr_t * bytes_written )
{
    struct pci_io io;

#if HAVE_PCI_IO_PC_DOMAIN
    io.pi_sel.pc_domain = dev->domain;
#endif
    io.pi_sel.pc_bus = dev->bus;
    io.pi_sel.pc_dev = dev->dev;
    io.pi_sel.pc_func = dev->func;

    *bytes_written = 0;
    while ( size > 0 ) {
	int towrite = (size < 4 ? size : 4);

	/* Only power of two allowed. */
	if (towrite == 3)
	    towrite = 2;

	io.pi_reg = offset;
	io.pi_width = towrite;
	memcpy( &io.pi_data, data, towrite );

	if ( ioctl( freebsd_pci_sys->pcidev, PCIOCWRITE, &io ) < 0 )
	    return errno;

	offset += towrite;
	data = (char *)data + towrite;
	size -= towrite;
	*bytes_written += towrite;
    }

    return 0;
}

/**
 * Read a VGA rom using the 0xc0000 mapping.
 *
 * This function should be extended to handle access through PCI resources,
 * which should be more reliable when available.
 */
static int
pci_device_freebsd_read_rom( struct pci_device * dev, void * buffer )
{
    struct pci_device_private *priv = (struct pci_device_private *) dev;
    void *bios;
    pciaddr_t rom_base;
    uint32_t rom;
    uint16_t reg;
    int pci_rom, memfd;

    if ( ( dev->device_class & 0x00ffff00 ) !=
	 ( ( PCIC_DISPLAY << 16 ) | ( PCIS_DISPLAY_VGA << 8 ) ) )
    {
	return ENOSYS;
    }

    if (priv->rom_base == 0) {
#if defined(__amd64__) || defined(__i386__)
	rom_base = 0xc0000;
	pci_rom = 0;
#else
	return ENOSYS;
#endif
    } else {
	rom_base = priv->rom_base;
	pci_rom = 1;

	pci_device_cfg_read_u16( dev, &reg, PCIR_COMMAND );
	pci_device_cfg_write_u16( dev, reg | PCIM_CMD_MEMEN, PCIR_COMMAND );
	pci_device_cfg_read_u32( dev, &rom, PCIR_BIOS );
	pci_device_cfg_write_u32( dev, rom | PCIM_BIOS_ENABLE, PCIR_BIOS );
    }

    printf("Using rom_base = 0x%lx\n", (long)rom_base);
    memfd = open( "/dev/mem", O_RDONLY );
    if ( memfd == -1 )
	return errno;

    bios = mmap( NULL, dev->rom_size, PROT_READ, 0, memfd, rom_base );
    if ( bios == MAP_FAILED ) {
	close( memfd );
	return errno;
    }

    memcpy( buffer, bios, dev->rom_size );

    munmap( bios, dev->rom_size );
    close( memfd );

    if (pci_rom) {
	pci_device_cfg_write_u32( dev, PCIR_BIOS, rom );
	pci_device_cfg_write_u16( dev, PCIR_COMMAND, reg );
    }

    return 0;
}

/** Returns the number of regions (base address registers) the device has */

static int
pci_device_freebsd_get_num_regions( struct pci_device * dev )
{
    struct pci_device_private *priv = (struct pci_device_private *) dev;

    switch (priv->header_type) {
    case 0:
	return 6;
    case 1:
	return 2;
    case 2:
	return 1;
    default:
	printf("unknown header type %02x\n", priv->header_type);
	return 0;
    }
}

#ifdef PCIOCGETBAR

static int
pci_device_freebsd_probe( struct pci_device * dev )
{
    struct pci_device_private *priv = (struct pci_device_private *) dev;
    struct pci_bar_io bar;
    uint8_t irq;
    int err, i;

#if HAVE_PCI_IO_PC_DOMAIN
    bar.pbi_sel.pc_domain = dev->domain;
#endif
    bar.pbi_sel.pc_bus = dev->bus;
    bar.pbi_sel.pc_dev = dev->dev;
    bar.pbi_sel.pc_func = dev->func;


    /* Many of the fields were filled in during initial device enumeration.
     * At this point, we need to fill in regions, rom_size, and irq.
     */

    err = pci_device_cfg_read_u8( dev, &irq, 60 );
    if (err)
	return errno;
    dev->irq = irq;

    for (i = 0; i < pci_device_freebsd_get_num_regions( dev ); i++) {
	bar.pbi_reg = PCIR_BAR(i);
	if ( ioctl( freebsd_pci_sys->pcidev, PCIOCGETBAR, &bar ) < 0 )
	    continue;

	if (PCI_BAR_IO(bar.pbi_base))
	    dev->regions[i].is_IO = 1;

	if ((bar.pbi_base & PCIM_BAR_MEM_TYPE) == PCIM_BAR_MEM_64)
	    dev->regions[i].is_64 = 1;

	if (bar.pbi_base & PCIM_BAR_MEM_PREFETCH)
	    dev->regions[i].is_prefetchable = 1;

	dev->regions[i].base_addr = bar.pbi_base & ~((uint64_t)0xf);
	dev->regions[i].size = bar.pbi_length;
    }

    /* If it's a VGA device, set up the rom size for read_rom using the
     * 0xc0000 mapping.
     */
     if ((dev->device_class & 0x00ffff00) ==
	((PCIC_DISPLAY << 16) | (PCIS_DISPLAY_VGA << 8))) {
	     dev->rom_size = 64 * 1024;
     }

     return 0;
}

#else

/** Masks out the flag bigs of the base address register value */
static uint32_t
get_map_base( uint32_t val )
{
    if (val & 0x01)
	return val & ~0x03;
    else
	return val & ~0x0f;
}

/** Returns the size of a region based on the all-ones test value */
static int
get_test_val_size( uint32_t testval )
{
    if (testval == 0)
	return 0;

    /* Mask out the flag bits */
    testval = get_map_base( testval );

    return 1 << (ffs(testval) - 1);
}

/**
 * Sets the address and size information for the region from config space
 * registers.
 *
 * This would be much better provided by a kernel interface.
 *
 * \return 0 on success, or an errno value.
 */
static int
pci_device_freebsd_get_region_info( struct pci_device * dev, int region,
				    int bar )
{
    uint32_t addr, testval;
    uint16_t cmd;
    int err;

    /* Get the base address */
    err = pci_device_cfg_read_u32( dev, &addr, bar );
    if (err != 0)
	return err;

    /*
     * We are going to be doing evil things to the registers here
     * so disable them via the command register first.
     */
    err = pci_device_cfg_read_u16( dev, &cmd, PCIR_COMMAND );
    if (err != 0)
	return err;

    err = pci_device_cfg_write_u16( dev,
	cmd & ~(PCI_BAR_MEM(addr) ? PCIM_CMD_MEMEN : PCIM_CMD_PORTEN),
	PCIR_COMMAND );
    if (err != 0)
	return err;

    /* Test write all ones to the register, then restore it. */
    err = pci_device_cfg_write_u32( dev, 0xffffffff, bar );
    if (err != 0)
	return err;
    err = pci_device_cfg_read_u32( dev, &testval, bar );
    if (err != 0)
	return err;
    err = pci_device_cfg_write_u32( dev, addr, bar );
    if (err != 0)
	return err;

    /* Restore the command register */
    err = pci_device_cfg_write_u16( dev, cmd, PCIR_COMMAND );
    if (err != 0)
	return err;

    if (addr & 0x01)
	dev->regions[region].is_IO = 1;
    if (addr & 0x04)
	dev->regions[region].is_64 = 1;
    if (addr & 0x08)
	dev->regions[region].is_prefetchable = 1;

    /* Set the size */
    dev->regions[region].size = get_test_val_size( testval );
	printf("size = 0x%lx\n", (long)dev->regions[region].size);

    /* Set the base address value */
    if (dev->regions[region].is_64) {
	uint32_t top;

	err = pci_device_cfg_read_u32( dev, &top, bar + 4 );
	if (err != 0)
	    return err;

	dev->regions[region].base_addr = ((uint64_t)top << 32) |
					  get_map_base(addr);
    } else {
	dev->regions[region].base_addr = get_map_base(addr);
    }

    return 0;
}

static int
pci_device_freebsd_probe( struct pci_device * dev )
{
    struct pci_device_private *priv = (struct pci_device_private *) dev;
    uint32_t reg, size;
    uint8_t irq;
    int err, i, bar;

    /* Many of the fields were filled in during initial device enumeration.
     * At this point, we need to fill in regions, rom_size, and irq.
     */

    err = pci_device_cfg_read_u8( dev, &irq, 60 );
    if (err)
	return errno;
    dev->irq = irq;

    bar = 0x10;
    for (i = 0; i < pci_device_freebsd_get_num_regions( dev ); i++) {
	pci_device_freebsd_get_region_info( dev, i, bar );
	if (dev->regions[i].is_64) {
	    bar += 0x08;
	    i++;
	} else
	    bar += 0x04;
    }

    /* If it's a VGA device, set up the rom size for read_rom */
    if ((dev->device_class & 0x00ffff00) ==
	((PCIC_DISPLAY << 16) | (PCIS_DISPLAY_VGA << 8)))
    {
	err = pci_device_cfg_read_u32( dev, &reg, PCIR_BIOS );
	if (err)
	    return errno;

	if (reg == 0) {
	    dev->rom_size = 0x10000;
	    return 0;
	}

	err = pci_device_cfg_write_u32( dev, ~PCIM_BIOS_ENABLE, PCIR_BIOS );
	if (err)
	    return errno;
	pci_device_cfg_read_u32( dev, &size, PCIR_BIOS );
	pci_device_cfg_write_u32( dev, reg, PCIR_BIOS );

	if ((reg & PCIM_BIOS_ADDR_MASK) != 0) {
	    priv->rom_base = (reg & PCIM_BIOS_ADDR_MASK);
	    dev->rom_size = -(size & PCIM_BIOS_ADDR_MASK);
	}
    }

    return 0;
}

#endif

static void
pci_system_freebsd_destroy(void)
{
    close(freebsd_pci_sys->pcidev);
    free(freebsd_pci_sys->pci_sys.devices);
    freebsd_pci_sys = NULL;
}

static const struct pci_system_methods freebsd_pci_methods = {
    .destroy = pci_system_freebsd_destroy,
    .destroy_device = NULL, /* nothing to do for this */
    .read_rom = pci_device_freebsd_read_rom,
    .probe = pci_device_freebsd_probe,
    .map_range = pci_device_freebsd_map_range,
    .unmap_range = pci_device_freebsd_unmap_range,
    .read = pci_device_freebsd_read,
    .write = pci_device_freebsd_write,
    .fill_capabilities = pci_fill_capabilities_generic,
};

/**
 * Attempt to access the FreeBSD PCI interface.
 */
_pci_hidden int
pci_system_freebsd_create( void )
{
    struct pci_conf_io pciconfio;
    struct pci_conf pciconf[255];
    int pcidev;
    int i;

    /* Try to open the PCI device */
    pcidev = open( "/dev/pci", O_RDWR );
    if ( pcidev == -1 )
	return ENXIO;

    freebsd_pci_sys = calloc( 1, sizeof( struct freebsd_pci_system ) );
    if ( freebsd_pci_sys == NULL ) {
	close( pcidev );
	return ENOMEM;
    }
    pci_sys = &freebsd_pci_sys->pci_sys;

    pci_sys->methods = & freebsd_pci_methods;
    freebsd_pci_sys->pcidev = pcidev;

    /* Probe the list of devices known by the system */
    bzero( &pciconfio, sizeof( struct pci_conf_io ) );
    pciconfio.match_buf_len = sizeof(pciconf);
    pciconfio.matches = pciconf;

    if ( ioctl( pcidev, PCIOCGETCONF, &pciconfio ) == -1) {
	free( pci_sys );
	close( pcidev );
	return errno;
    }

    if (pciconfio.status == PCI_GETCONF_ERROR ) {
	free( pci_sys );
	close( pcidev );
	return EINVAL;
    }

    /* Translate the list of devices into pciaccess's format. */
    pci_sys->num_devices = pciconfio.num_matches;
    pci_sys->devices = calloc( pciconfio.num_matches,
			       sizeof( struct pci_device_private ) );

    for ( i = 0; i < pciconfio.num_matches; i++ ) {
	struct pci_conf *p = &pciconf[ i ];

#if HAVE_PCI_IO_PC_DOMAIN
	pci_sys->devices[ i ].base.domain = p->pc_sel.pc_domain;
#else
	pci_sys->devices[ i ].base.domain = 0;
#endif
	pci_sys->devices[ i ].base.bus = p->pc_sel.pc_bus;
	pci_sys->devices[ i ].base.dev = p->pc_sel.pc_dev;
	pci_sys->devices[ i ].base.func = p->pc_sel.pc_func;
	pci_sys->devices[ i ].base.vendor_id = p->pc_vendor;
	pci_sys->devices[ i ].base.device_id = p->pc_device;
	pci_sys->devices[ i ].base.subvendor_id = p->pc_subvendor;
	pci_sys->devices[ i ].base.subdevice_id = p->pc_subdevice;
	pci_sys->devices[ i ].base.revision = p->pc_revid;
	pci_sys->devices[ i ].base.device_class = (uint32_t)p->pc_class << 16 |
	    (uint32_t)p->pc_subclass << 8 | (uint32_t)p->pc_progif;
	pci_sys->devices[ i ].header_type = p->pc_hdr & 0x7f;
    }

    return 0;
}
