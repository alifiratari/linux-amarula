/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef __XFS_SB_H__
#define	__XFS_SB_H__

struct xfs_mount;
struct xfs_sb;
struct xfs_dsb;
struct xfs_trans;
struct xfs_fsop_geom;
struct xfs_perag;

/*
 * perag get/put wrappers for ref counting
 */
extern struct xfs_perag *xfs_perag_get(struct xfs_mount *, xfs_agnumber_t);
extern struct xfs_perag *xfs_perag_get_tag(struct xfs_mount *, xfs_agnumber_t,
					   int tag);
extern void	xfs_perag_put(struct xfs_perag *pag);
extern int	xfs_initialize_perag_data(struct xfs_mount *, xfs_agnumber_t);

extern void	xfs_log_sb(struct xfs_trans *tp);
extern int	xfs_sync_sb(struct xfs_mount *mp, bool wait);
extern int	xfs_sync_sb_buf(struct xfs_mount *mp);
extern void	xfs_sb_mount_common(struct xfs_mount *mp, struct xfs_sb *sbp);
extern void	xfs_sb_from_disk(struct xfs_sb *to, struct xfs_dsb *from);
extern void	xfs_sb_to_disk(struct xfs_dsb *to, struct xfs_sb *from);
extern void	xfs_sb_quota_from_disk(struct xfs_sb *sbp);

extern int	xfs_update_secondary_sbs(struct xfs_mount *mp);

#define XFS_FS_GEOM_MAX_STRUCT_VER	(4)
extern int	xfs_fs_geometry(struct xfs_sb *sbp, struct xfs_fsop_geom *geo,
				int struct_version);
extern int	xfs_sb_read_secondary(struct xfs_mount *mp,
				struct xfs_trans *tp, xfs_agnumber_t agno,
				struct xfs_buf **bpp);

#endif	/* __XFS_SB_H__ */
