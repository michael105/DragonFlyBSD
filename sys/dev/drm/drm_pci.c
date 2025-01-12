/*
 * Copyright 2003 José Fonseca.
 * Copyright 2003 Leif Delgass.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <drm/drmP.h>
#include "drm_internal.h"
#include "drm_legacy.h"

/**
 * drm_pci_alloc - Allocate a PCI consistent memory block, for DMA.
 * @dev: DRM device
 * @size: size of block to allocate
 * @align: alignment of block
 *
 * Return: A handle to the allocated memory block on success or NULL on
 * failure.
 */
drm_dma_handle_t *drm_pci_alloc(struct drm_device * dev, size_t size, size_t align)
{
	drm_dma_handle_t *dmah;
	unsigned long addr;
	size_t sz;

	/* pci_alloc_consistent only guarantees alignment to the smallest
	 * PAGE_SIZE order which is greater than or equal to the requested size.
	 * Return NULL here for now to make sure nobody tries for larger alignment
	 */
	if (align > size)
		return NULL;

	dmah = kmalloc(sizeof(drm_dma_handle_t), M_DRM, M_WAITOK | M_NULLOK);
	if (!dmah)
		return NULL;

	dmah->size = size;
	dmah->vaddr = dma_alloc_coherent(&dev->pdev->dev, size, &dmah->busaddr, GFP_KERNEL);

	if (dmah->vaddr == NULL) {
		kfree(dmah);
		return NULL;
	}

	memset(dmah->vaddr, 0, size);

	/* XXX - Is virt_to_page() legal for consistent mem? */
	/* Reserve */
	for (addr = (unsigned long)dmah->vaddr, sz = size;
	     sz > 0; addr += PAGE_SIZE, sz -= PAGE_SIZE) {
#if 0
		SetPageReserved(virt_to_page((void *)addr));
#endif
	}

	return dmah;
}

EXPORT_SYMBOL(drm_pci_alloc);

/*
 * Free a PCI consistent memory block without freeing its descriptor.
 *
 * This function is for internal use in the Linux-specific DRM core code.
 */
void __drm_legacy_pci_free(struct drm_device * dev, drm_dma_handle_t * dmah)
{
	unsigned long addr;
	size_t sz;

	if (dmah->vaddr) {
		/* XXX - Is virt_to_page() legal for consistent mem? */
		/* Unreserve */
		for (addr = (unsigned long)dmah->vaddr, sz = dmah->size;
		     sz > 0; addr += PAGE_SIZE, sz -= PAGE_SIZE) {
#if 0
			ClearPageReserved(virt_to_page((void *)addr));
#endif
		}
		dma_free_coherent(&dev->pdev->dev, dmah->size, dmah->vaddr,
				  dmah->busaddr);
	}
}

/**
 * drm_pci_free - Free a PCI consistent memory block
 * @dev: DRM device
 * @dmah: handle to memory block
 */
void drm_pci_free(struct drm_device * dev, drm_dma_handle_t * dmah)
{
	__drm_legacy_pci_free(dev, dmah);
	kfree(dmah);
}

EXPORT_SYMBOL(drm_pci_free);

#ifdef CONFIG_PCI

static int drm_get_pci_domain(struct drm_device *dev)
{
#ifndef __alpha__
	/* For historical reasons, drm_get_pci_domain() is busticated
	 * on most archs and has to remain so for userspace interface
	 * < 1.4, except on alpha which was right from the beginning
	 */
	if (dev->if_version < 0x10004)
		return 0;
#endif /* __alpha__ */

#if 0
	return pci_domain_nr(dev->pdev->bus);
#else
	return dev->pci_domain;
#endif
}

int drm_pci_set_busid(struct drm_device *dev, struct drm_master *master)
{
	master->unique = kasprintf(GFP_KERNEL, "pci:%04x:%02x:%02x.%d",
					drm_get_pci_domain(dev),
					dev->pdev->bus->number,
					PCI_SLOT(dev->pdev->devfn),
					PCI_FUNC(dev->pdev->devfn));
	if (!master->unique)
		return -ENOMEM;

	master->unique_len = strlen(master->unique);
	return 0;
}
EXPORT_SYMBOL(drm_pci_set_busid);

int drm_pci_set_unique(struct drm_device *dev,
		       struct drm_master *master,
		       struct drm_unique *u)
{
	int domain, bus, slot, func, ret;

	master->unique_len = u->unique_len;
	master->unique = kmalloc(master->unique_len + 1, M_DRM, M_NOWAIT);
	if (!master->unique) {
		ret = -ENOMEM;
		goto err;
	}

	if (copy_from_user(master->unique, u->unique, master->unique_len)) {
		ret = -EFAULT;
		goto err;
	}

	master->unique[master->unique_len] = '\0';

	/* Return error if the busid submitted doesn't match the device's actual
	 * busid.
	 */
	ret = ksscanf(master->unique, "PCI:%d:%d:%d", &bus, &slot, &func);
	if (ret != 3) {
		ret = -EINVAL;
		goto err;
	}

	domain = bus >> 8;
	bus &= 0xff;

	if ((domain != drm_get_pci_domain(dev)) ||
	    (bus != dev->pdev->bus->number) ||
	    (slot != PCI_SLOT(dev->pdev->devfn)) ||
	    (func != PCI_FUNC(dev->pdev->devfn))) {
		ret = -EINVAL;
		goto err;
	}
	return 0;
err:
	return ret;
}

static int drm_pci_irq_by_busid(struct drm_device *dev, struct drm_irq_busid *p)
{
	if ((p->busnum >> 8) != dev->pci_domain ||
	    (p->busnum & 0xff) != dev->pci_bus ||
	    p->devnum != dev->pci_slot || p->funcnum != dev->pci_func)
		return -EINVAL;

	p->irq = dev->pdev->irq;

	DRM_DEBUG("%d:%d:%d => IRQ %d\n", p->busnum, p->devnum, p->funcnum,
		  p->irq);
	return 0;
}

#ifdef __DragonFly__
int
drm_getpciinfo(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_pciinfo *info = data;

	info->domain = 0;
	info->bus = dev->pci_bus;
	info->dev = PCI_SLOT(dev->pdev->devfn);
	info->func = PCI_FUNC(dev->pdev->devfn);
	info->vendor_id = dev->pdev->vendor;
	info->device_id = dev->pdev->device;
	info->subvendor_id = dev->pdev->subsystem_vendor;
	info->subdevice_id = dev->pdev->subsystem_device;
	info->revision_id = 0;

	return 0;
}
#endif

/**
 * drm_irq_by_busid - Get interrupt from bus ID
 * @dev: DRM device
 * @data: IOCTL parameter pointing to a drm_irq_busid structure
 * @file_priv: DRM file private.
 *
 * Finds the PCI device with the specified bus id and gets its IRQ number.
 * This IOCTL is deprecated, and will now return EINVAL for any busid not equal
 * to that of the device that this DRM instance attached to.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_irq_by_busid(struct drm_device *dev, void *data,
		     struct drm_file *file_priv)
{
	struct drm_irq_busid *p = data;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	/* UMS was only ever support on PCI devices. */
	if (WARN_ON(!dev->pdev))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_HAVE_IRQ))
		return -EINVAL;

	return drm_pci_irq_by_busid(dev, p);
}

/**
 * drm_get_pci_dev - Register a PCI device with the DRM subsystem
 * @pdev: PCI device
 * @ent: entry from the PCI ID table that matches @pdev
 * @driver: DRM device driver
 *
 * Attempt to gets inter module "drm" information. If we are first
 * then register the character device and inter module information.
 * Try and register, if we fail to register, backout previous work.
 *
 * NOTE: This function is deprecated, please use drm_dev_alloc() and
 * drm_dev_register() instead and remove your ->load() callback.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_get_pci_dev(struct pci_dev *pdev, const struct pci_device_id *ent,
		    struct drm_driver *driver)
{
	struct drm_device *dev;
	int ret;

	DRM_DEBUG("\n");

	dev = drm_dev_alloc(driver, &pdev->dev);
	if (!dev)
		return -ENOMEM;

#if 0
	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;
#endif

	dev->pdev = pdev;
#ifdef __alpha__
	dev->hose = pdev->sysdata;
#endif

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		pci_set_drvdata(pdev, dev);

#if 0
	drm_pci_agp_init(dev);
#endif

	ret = drm_dev_register(dev, ent->driver_data);
	if (ret)
		goto err_agp;

	DRM_INFO("Initialized %s %d.%d.%d %s for %s on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 driver->date, pci_name(pdev), dev->primary->index);

	/* No locking needed since shadow-attach is single-threaded since it may
	 * only be called from the per-driver module init hook. */
	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		list_add_tail(&dev->legacy_dev_list, &driver->legacy_dev_list);

	return 0;

err_agp:
#if 0
	drm_pci_agp_destroy(dev);
	pci_disable_device(pdev);
err_free:
	drm_dev_unref(dev);
#endif
	return ret;
}
EXPORT_SYMBOL(drm_get_pci_dev);

/**
 * drm_pci_init - Register matching PCI devices with the DRM subsystem
 * @driver: DRM device driver
 * @pdriver: PCI device driver
 *
 * Initializes a drm_device structures, registering the stubs and initializing
 * the AGP device.
 *
 * NOTE: This function is deprecated. Modern modesetting drm drivers should use
 * pci_register_driver() directly, this function only provides shadow-binding
 * support for old legacy drivers on top of that core pci function.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_pci_init(struct drm_driver *driver, struct pci_driver *pdriver)
{
#if 0
	struct pci_dev *pdev = NULL;
	const struct pci_device_id *pid;
	int i;
#endif

	DRM_DEBUG("\n");

	if (driver->driver_features & DRIVER_MODESET)
		return pci_register_driver(pdriver);

#if 0
	/* If not using KMS, fall back to stealth mode manual scanning. */
	INIT_LIST_HEAD(&driver->legacy_dev_list);
	for (i = 0; pdriver->id_table[i].vendor != 0; i++) {
		pid = &pdriver->id_table[i];

		/* Loop around setting up a DRM device for each PCI device
		 * matching our ID and device class.  If we had the internal
		 * function that pci_get_subsys and pci_get_class used, we'd
		 * be able to just pass pid in instead of doing a two-stage
		 * thing.
		 */
		pdev = NULL;
		while ((pdev =
			pci_get_subsys(pid->vendor, pid->device, pid->subvendor,
				       pid->subdevice, pdev)) != NULL) {
			if ((pdev->class & pid->class_mask) != pid->class)
				continue;

			/* stealth mode requires a manual probe */
			pci_dev_get(pdev);
			drm_get_pci_dev(pdev, pid, driver);
		}
	}
#endif
	return 0;
}

int drm_pcie_get_speed_cap_mask(struct drm_device *dev, u32 *mask)
{
	device_t root;
	int pos;
	u32 lnkcap = 0, lnkcap2 = 0;

	*mask = 0;
	if (!dev->pdev)
		return -EINVAL;

	root = device_get_parent(dev->dev->bsddev);

	/* we've been informed via and serverworks don't make the cut */
	if (pci_get_vendor(root) == PCI_VENDOR_ID_VIA ||
	    pci_get_vendor(root) == PCI_VENDOR_ID_SERVERWORKS)
		return -EINVAL;

	pos = 0;
	pci_find_extcap(root, PCIY_EXPRESS, &pos);
	if (!pos)
		return -EINVAL;

	lnkcap = pci_read_config(root, pos + PCIER_LINKCAP, 4);
	lnkcap2 = pci_read_config(root, pos + PCIER_LINK_CAP2, 4);

	lnkcap &= PCIEM_LNKCAP_SPEED_MASK;
	lnkcap2 &= 0xfe;

#define	PCI_EXP_LNKCAP_SLS_2_5GB	PCIEM_LNKCAP_SPEED_2_5
#define	PCI_EXP_LNKCAP_SLS_5_0GB	PCIEM_LNKCAP_SPEED_5
#define	PCI_EXP_LNKCAP2_SLS_2_5GB 0x02	/* Supported Link Speed 2.5GT/s */
#define	PCI_EXP_LNKCAP2_SLS_5_0GB 0x04	/* Supported Link Speed 5.0GT/s */
#define	PCI_EXP_LNKCAP2_SLS_8_0GB 0x08	/* Supported Link Speed 8.0GT/s */

	if (lnkcap2) {	/* PCIe r3.0-compliant */
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_2_5GB)
			*mask |= DRM_PCIE_SPEED_25;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_5_0GB)
			*mask |= DRM_PCIE_SPEED_50;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_8_0GB)
			*mask |= DRM_PCIE_SPEED_80;
	} else {	/* pre-r3.0 */
		if (lnkcap & PCI_EXP_LNKCAP_SLS_2_5GB)
			*mask |= DRM_PCIE_SPEED_25;
		if (lnkcap & PCI_EXP_LNKCAP_SLS_5_0GB)
			*mask |= (DRM_PCIE_SPEED_25 | DRM_PCIE_SPEED_50);
	}

	DRM_INFO("probing gen 2 caps for device %x:%x = %x/%x\n", pci_get_vendor(root), pci_get_device(root), lnkcap, lnkcap2);
	return 0;
}
EXPORT_SYMBOL(drm_pcie_get_speed_cap_mask);

static int
pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *dst)
{
	if (pos & 3)
		return -EINVAL;

	if (!pcie_capability_reg_implemented(dev, pos))
		return -EINVAL;

	return pci_read_config_dword(dev, pci_pcie_cap(dev) + pos, dst);
}

int drm_pcie_get_max_link_width(struct drm_device *dev, u32 *mlw)
{
	struct pci_dev *root;
	u32 lnkcap = 0;

	*mlw = 0;
	if (!dev->pdev)
		return -EINVAL;

	root = dev->pdev->bus->self;

	pcie_capability_read_dword(root, PCI_EXP_LNKCAP, &lnkcap);

	*mlw = (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4;

	DRM_INFO("probing mlw for device %x:%x = %x\n", root->vendor, root->device, lnkcap);
	return 0;
}
EXPORT_SYMBOL(drm_pcie_get_max_link_width);

#else


int drm_pci_init(struct drm_driver *driver, struct pci_driver *pdriver)
{
	return -1;
}

void drm_pci_agp_destroy(struct drm_device *dev) {}

int drm_irq_by_busid(struct drm_device *dev, void *data,
		     struct drm_file *file_priv)
{
	return -EINVAL;
}

int drm_pci_set_unique(struct drm_device *dev,
		       struct drm_master *master,
		       struct drm_unique *u)
{
	return -EINVAL;
}
#endif

EXPORT_SYMBOL(drm_pci_init);
