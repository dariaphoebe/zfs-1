/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Portions Copyright 2007 Apple Inc. All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev_disk.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/sunldi.h>
#endif /*__APPLE__*/

/*
 * Virtual device vector for disks.
 */

#ifndef __APPLE__
extern ldi_ident_t zfs_li;

typedef struct vdev_disk_buf {
	buf_t	vdb_buf;
	zio_t	*vdb_io;
} vdev_disk_buf_t;
#endif /*!__APPLE__*/

static int
vdev_disk_open(vdev_t *vd, uint64_t *psize, uint64_t *ashift)
{
	vdev_disk_t *dvd = NULL;
#ifdef __APPLE__
	vnode_t *devvp = NULLVP;
	vfs_context_t context = NULL;
	uint64_t blkcnt;
	uint32_t blksize;
	int fmode = 0;
#else
	struct dk_minfo dkm;
	dev_t dev;
	char *physpath, *minorname;
	int otyp;
#endif
	int error;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (EINVAL);
	}

	dvd = kmem_zalloc(sizeof (vdev_disk_t), KM_SLEEP);

	/*
	 * When opening a disk device, we want to preserve the user's original
	 * intent.  We always want to open the device by the path the user gave
	 * us, even if it is one of multiple paths to the save device.  But we
	 * also want to be able to survive disks being removed/recabled.
	 * Therefore the sequence of opening devices is:
	 *
	 * 1. Try opening the device by path.  For legacy pools without the
	 *    'whole_disk' property, attempt to fix the path by appending 's0'.
	 *
	 * 2. If the devid of the device matches the stored value, return
	 *    success.
	 *
	 * 3. Otherwise, the device may have moved.  Try opening the device
	 *    by the devid instead.
	 *
	 */

#ifdef __APPLE__
	/* ### APPLE TODO ### */
	/* ddi_devid_str_decode */

	context = vfs_context_create((vfs_context_t)0);

	/* Obtain an opened/referenced vnode for the device. */
	if ((error = vnode_open(vd->vdev_path, spa_mode, 0, 0, &devvp, context))) {
		goto out;
	}
	if (!vnode_isblk(devvp)) {
		error = ENOTBLK;
		goto out;
	}
	/* ### APPLE TODO ### */
	/* vnode_authorize devvp for KAUTH_VNODE_READ_DATA and
	 * KAUTH_VNODE_WRITE_DATA
	 */

	/*
	 * Disallow opening of a device that is currently in use.
	 * Flush out any old buffers remaining from a previous use.
	 */
	if ((error = vfs_mountedon(devvp))) {
		goto out;
	}
	if (VNOP_FSYNC(devvp, MNT_WAIT, context) != 0) {
		error = ENOTBLK;
		goto out;
	}
	if ((error = buf_invalidateblks(devvp, BUF_WRITE_DATA, 0, 0))) {
		goto out;
	}

	/*
	 * Determine the actual size of the device.
	 */
	if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&blksize, 0, context)
	       	!= 0 || VNOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT, (caddr_t)&blkcnt,
		0, context) != 0) {

		error = EINVAL;
		goto out;
	}
	*psize = blkcnt * (uint64_t)blksize;

	/*
	 *  ### APPLE TODO ###
	 * If we own the whole disk, try to enable disk write caching.
	 */

	/*
	 * Take the device's minimum transfer size into account.
	 */
	*ashift = highbit(MAX(blksize, SPA_MINBLOCKSIZE)) - 1;

	/*
	 * Clear the nowritecache bit, so that on a vdev_reopen() we will
	 * try again.
	 */
	vd->vdev_nowritecache = B_FALSE;
	vd->vdev_tsd = dvd;
	dvd->vd_devvp = devvp;
out:
	if (error) {
		if (devvp)
			vnode_close(devvp, fmode, context);
		if (dvd)
			kmem_free(dvd, sizeof (vdev_disk_t));

		/*
		 * Since the open has failed, vd->vdev_tsd should
		 * be NULL when we get here, signaling to the
		 * rest of the spa not to try and reopen or close this device
		 */
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
	}
	if (context) {
		(void) vfs_context_rele(context);
	}
	return (error);
#else
	if (vd->vdev_devid != NULL) {
		if (ddi_devid_str_decode(vd->vdev_devid, &dvd->vd_devid,
		    &dvd->vd_minor) != 0) {
			vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
			return (EINVAL);
		}
	}

	error = EINVAL;		/* presume failure */

	if (vd->vdev_path != NULL) {
		ddi_devid_t devid;

		if (vd->vdev_wholedisk == -1ULL) {
			size_t len = strlen(vd->vdev_path) + 3;
			char *buf = kmem_alloc(len, KM_SLEEP);
			ldi_handle_t lh;

			(void) snprintf(buf, len, "%ss0", vd->vdev_path);

			if (ldi_open_by_name(buf, spa_mode, kcred,
			    &lh, zfs_li) == 0) {
				spa_strfree(vd->vdev_path);
				vd->vdev_path = buf;
				vd->vdev_wholedisk = 1ULL;
				(void) ldi_close(lh, spa_mode, kcred);
			} else {
				kmem_free(buf, len);
			}
		}

		error = ldi_open_by_name(vd->vdev_path, spa_mode, kcred,
		    &dvd->vd_lh, zfs_li);

		/*
		 * Compare the devid to the stored value.
		 */
		if (error == 0 && vd->vdev_devid != NULL &&
		    ldi_get_devid(dvd->vd_lh, &devid) == 0) {
			if (ddi_devid_compare(devid, dvd->vd_devid) != 0) {
				error = EINVAL;
				(void) ldi_close(dvd->vd_lh, spa_mode, kcred);
				dvd->vd_lh = NULL;
			}
			ddi_devid_free(devid);
		}

		/*
		 * If we succeeded in opening the device, but 'vdev_wholedisk'
		 * is not yet set, then this must be a slice.
		 */
		if (error == 0 && vd->vdev_wholedisk == -1ULL)
			vd->vdev_wholedisk = 0;
	}

	/*
	 * If we were unable to open by path, or the devid check fails, open by
	 * devid instead.
	 */
	if (error != 0 && vd->vdev_devid != NULL)
		error = ldi_open_by_devid(dvd->vd_devid, dvd->vd_minor,
		    spa_mode, kcred, &dvd->vd_lh, zfs_li);

	/*
	 * If all else fails, then try opening by physical path (if available)
	 * or the logical path (if we failed due to the devid check).  While not
	 * as reliable as the devid, this will give us something, and the higher
	 * level vdev validation will prevent us from opening the wrong device.
	 */
	if (error) {
		if (vd->vdev_physpath != NULL &&
		    (dev = ddi_pathname_to_dev_t(vd->vdev_physpath)) != ENODEV)
			error = ldi_open_by_dev(&dev, OTYP_BLK, spa_mode,
			    kcred, &dvd->vd_lh, zfs_li);

		/*
		 * Note that we don't support the legacy auto-wholedisk support
		 * as above.  This hasn't been used in a very long time and we
		 * don't need to propagate its oddities to this edge condition.
		 */
		if (error && vd->vdev_path != NULL)
			error = ldi_open_by_name(vd->vdev_path, spa_mode, kcred,
			    &dvd->vd_lh, zfs_li);
	}

	if (error) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (error);
	}

	/*
	 * Once a device is opened, verify that the physical device path (if
	 * available) is up to date.
	 */
	if (ldi_get_dev(dvd->vd_lh, &dev) == 0 &&
	    ldi_get_otyp(dvd->vd_lh, &otyp) == 0) {
		physpath = kmem_alloc(MAXPATHLEN, KM_SLEEP);
		minorname = NULL;
		if (ddi_dev_pathname(dev, otyp, physpath) == 0 &&
		    ldi_get_minor_name(dvd->vd_lh, &minorname) == 0 &&
		    (vd->vdev_physpath == NULL ||
		    strcmp(vd->vdev_physpath, physpath) != 0)) {
			if (vd->vdev_physpath)
				spa_strfree(vd->vdev_physpath);
			(void) strlcat(physpath, ":", MAXPATHLEN);
			(void) strlcat(physpath, minorname, MAXPATHLEN);
			vd->vdev_physpath = spa_strdup(physpath);
		}
		if (minorname)
			kmem_free(minorname, strlen(minorname) + 1);
		kmem_free(physpath, MAXPATHLEN);
	}

	/*
	 * Determine the actual size of the device.
	 */
	if (ldi_get_size(dvd->vd_lh, psize) != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (EINVAL);
	}

	/*
	 * If we own the whole disk, try to enable disk write caching.
	 * We ignore errors because it's OK if we can't do it.
	 */
	if (vd->vdev_wholedisk == 1) {
		int wce = 1;
		(void) ldi_ioctl(dvd->vd_lh, DKIOCSETWCE, (intptr_t)&wce,
		    FKIOCTL, kcred, NULL);
	}

	/*
	 * Determine the device's minimum transfer size.
	 * If the ioctl isn't supported, assume DEV_BSIZE.
	 */
	if (ldi_ioctl(dvd->vd_lh, DKIOCGMEDIAINFO, (intptr_t)&dkm,
	    FKIOCTL, kcred, NULL) != 0)
		dkm.dki_lbsize = DEV_BSIZE;

	*ashift = highbit(MAX(dkm.dki_lbsize, SPA_MINBLOCKSIZE)) - 1;

	/*
	 * Clear the nowritecache bit, so that on a vdev_reopen() we will
	 * try again.
	 */
	vd->vdev_nowritecache = B_FALSE;

	return (0);
#endif
}

static void
vdev_disk_close(vdev_t *vd)
{
	vdev_disk_t *dvd = vd->vdev_tsd;

	if (dvd == NULL)
		return;

#ifdef __APPLE__
	if (dvd->vd_devvp != NULL) {
		vfs_context_t context;
		int fmode;

		context = vfs_context_create((vfs_context_t)0);

		(void) vnode_close(dvd->vd_devvp, spa_mode, context);
		(void) vfs_context_rele(context);
	}
#else
	if (dvd->vd_minor != NULL)
		ddi_devid_str_free(dvd->vd_minor);

	if (dvd->vd_devid != NULL)
		ddi_devid_free(dvd->vd_devid);

	if (dvd->vd_lh != NULL)
		(void) ldi_close(dvd->vd_lh, spa_mode, kcred);
#endif /* __APPLE__ */

	kmem_free(dvd, sizeof (vdev_disk_t));
	vd->vdev_tsd = NULL;
}

static void
#ifdef __APPLE__
vdev_disk_io_intr(struct buf *bp, void *arg)
#else
vdev_disk_io_intr(buf_t *bp)
#endif /* __APPLE__ */
{
#ifdef __APPLE__
	zio_t *zio = (zio_t *)arg;

	if ((zio->io_error = buf_error(bp)) == 0 && buf_resid(bp) != 0) {
		zio->io_error = EIO;
	}
	buf_free(bp);
#else
	vdev_disk_buf_t *vdb = (vdev_disk_buf_t *)bp;
	zio_t *zio = vdb->vdb_io;

	if ((zio->io_error = geterror(bp)) == 0 && bp->b_resid != 0)
		zio->io_error = EIO;

	kmem_free(vdb, sizeof (vdev_disk_buf_t));

#endif /* __APPLE__ */
	//zio_next_stage_async(zio);
    zio_interrupt(zio);
}

static void
vdev_disk_ioctl_done(void *zio_arg, int error)
{
	zio_t *zio = zio_arg;

	zio->io_error = error;

	//zio_next_stage_async(zio);
    zio_interrupt(zio);
}

static int
vdev_disk_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_disk_t *dvd = vd->vdev_tsd;
#ifdef __APPLE__
	struct buf *bp;
	vfs_context_t context;
#else
	vdev_disk_buf_t *vdb;
	buf_t *bp;
#endif
	int flags, error;

    printf("vdev_disk_io_start\n");

	if (zio->io_type == ZIO_TYPE_IOCTL) {
		zio_vdev_io_bypass(zio);

		/* XXPOLICY */
		if (vdev_is_dead(vd)) {
			zio->io_error = ENXIO;
			//zio_next_stage_async(zio);
			return (ZIO_PIPELINE_CONTINUE);
            //return;
		}

		switch (zio->io_cmd) {

		case DKIOCFLUSHWRITECACHE:

			if (zfs_nocacheflush)
				break;

			if (vd->vdev_nowritecache) {
				zio->io_error = ENOTSUP;
				break;
			}

#ifdef __APPLE__
			context = vfs_context_create((vfs_context_t)0);
			error = VNOP_IOCTL(dvd->vd_devvp, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, context);
			(void) vfs_context_rele(context);
			if (error == 0)
				vdev_disk_ioctl_done(zio, error);
			else
				error = ENOTSUP;
#else
			zio->io_dk_callback.dkc_callback = vdev_disk_ioctl_done;
			zio->io_dk_callback.dkc_flag = FLUSH_VOLATILE;
			zio->io_dk_callback.dkc_cookie = zio;

			error = ldi_ioctl(dvd->vd_lh, zio->io_cmd,
			    (uintptr_t)&zio->io_dk_callback,
			    FKIOCTL, kcred, NULL);
#endif /* __APPLE__ */
			if (error == 0) {
				/*
				 * The ioctl will be done asychronously,
				 * and will call vdev_disk_ioctl_done()
				 * upon completion.
				 */
				return ZIO_PIPELINE_STOP;;
			} else if (error == ENOTSUP || error == ENOTTY) {
				/*
				 * If we get ENOTSUP or ENOTTY, we know that
				 * no future attempts will ever succeed.
				 * In this case we set a persistent bit so
				 * that we don't bother with the ioctl in the
				 * future.
				 */
				vd->vdev_nowritecache = B_TRUE;
			}
			zio->io_error = error;

			break;

		default:
			zio->io_error = ENOTSUP;
		}

		//zio_next_stage_async(zio);
        return (ZIO_PIPELINE_STOP);
	}

	if (zio->io_type == ZIO_TYPE_READ && vdev_cache_read(zio) == 0)
        return (ZIO_PIPELINE_STOP);
    //		return;

	if ((zio = vdev_queue_io(zio)) == NULL)
        return (ZIO_PIPELINE_STOP);
    //		return;

	flags = (zio->io_type == ZIO_TYPE_READ ? B_READ : B_WRITE);
#ifdef __APPLE__
	flags |= B_NOCACHE;
#else
	flags |= B_BUSY | B_NOCACHE;
#endif /* __APPLE__ */
	if (zio->io_flags & ZIO_FLAG_FAILFAST)
		flags |= B_FAILFAST;

#ifdef __APPLE__
	/*
	 * Check the state of this device to see if it has been offlined or
	 * is in an error state.  If the device was offlined or closed,
	 * dvd will be NULL and buf_alloc below will fail
	 */
	//error = vdev_is_dead(vd) ? ENXIO : vdev_error_inject(vd, zio);
	if (vdev_is_dead(vd))
        error = ENXIO;

	if (error) {
		zio->io_error = error;
		//zio_next_stage_async(zio);
		return (ZIO_PIPELINE_STOP);
	}

	bp = buf_alloc(dvd->vd_devvp);

	ASSERT(bp != NULL);
	ASSERT(zio->io_data != NULL);
	ASSERT(zio->io_size != 0);

	buf_setflags(bp, flags);
	buf_setcount(bp, zio->io_size);
	buf_setdataptr(bp, (uintptr_t)zio->io_data);
	buf_setlblkno(bp, lbtodb(zio->io_offset));
	buf_setblkno(bp, lbtodb(zio->io_offset));
	buf_setsize(bp, zio->io_size);
	if (buf_setcallback(bp, vdev_disk_io_intr, zio) != 0)
		panic("vdev_disk_io_start: buf_setcallback failed\n");

	if (zio->io_type == ZIO_TYPE_WRITE) {
		vnode_startwrite(dvd->vd_devvp);
	}
	error = VNOP_STRATEGY(bp);
#else
	vdb = kmem_alloc(sizeof (vdev_disk_buf_t), KM_SLEEP);

	vdb->vdb_io = zio;
	bp = &vdb->vdb_buf;

	bioinit(bp);
	bp->b_flags = flags;
	bp->b_bcount = zio->io_size;
	bp->b_un.b_addr = zio->io_data;
	bp->b_lblkno = lbtodb(zio->io_offset);
	bp->b_bufsize = zio->io_size;
	bp->b_iodone = (int (*)())vdev_disk_io_intr;

	/* XXPOLICY */
	//error = vdev_is_dead(vd) ? ENXIO : vdev_error_inject(vd, zio);
	if (vdev_is_dead(vd))
        error = ENXIO;

	if (error) {
		zio->io_error = error;
		bioerror(bp, error);
		bp->b_resid = bp->b_bcount;
		bp->b_iodone(bp);
		return (ZIO_PIPELINE_STOP);
	}

	error = ldi_strategy(dvd->vd_lh, bp);
	/* ldi_strategy() will return non-zero only on programming errors */
#endif /*__APPLE__*/
	ASSERT(error == 0);

    return (ZIO_PIPELINE_CONTINUE);
}

static void
vdev_disk_io_done(zio_t *zio)
{
	// vdev_t *vd = zio->io_vd;
	// vdev_disk_t *dvd = vd->vdev_tsd;
	int state;

	vdev_queue_io_done(zio);

	if (zio->io_type == ZIO_TYPE_WRITE)
		vdev_cache_write(zio);

	if (zio_injection_enabled && zio->io_error == 0)
		zio->io_error = zio_handle_device_injection(zio->io_vd, NULL, EIO);
#ifndef __APPLE__
	/*
	 * XXX- NOEL TODO
	 * If the device returned EIO, then attempt a DKIOCSTATE ioctl to see if
	 * the device has been removed.  If this is the case, then we trigger an
	 * asynchronous removal of the device.
	 */
	if (zio->io_error == EIO) {
		state = DKIO_NONE;
		if (ldi_ioctl(dvd->vd_lh, DKIOCSTATE, (intptr_t)&state,
		    FKIOCTL, kcred, NULL) == 0 &&
		    state != DKIO_INSERTED) {
			vd->vdev_remove_wanted = B_TRUE;
			spa_async_request(zio->io_spa, SPA_ASYNC_REMOVE);
		}
	}
#endif /* !__APPLE__ */

	//zio_next_stage(zio);
}

vdev_ops_t vdev_disk_ops = {
	vdev_disk_open,
	vdev_disk_close,
	vdev_default_asize,
	vdev_disk_io_start,
	vdev_disk_io_done,
	NULL,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};
