#include <linux/fs.h>
#include <linux/types.h>
#include "hmfs.h"
#include "hmfs_fs.h"
#include "node.h"

static struct kmem_cache *nat_entry_slab;

const struct address_space_operations hmfs_nat_aops;

static nid_t hmfs_max_nid(void)
{
	nid_t nid = 1;
	int height = 0;
	while (++height < NAT_TREE_MAX_HEIGHT)
		nid *= NAT_ADDR_PER_NODE;
	nid *= NAT_ENTRY_PER_BLOCK;
	return nid;
}

void set_new_dnode(struct dnode_of_data *dn, struct inode *inode,
		   struct hmfs_inode *hi, struct direct_node *db, nid_t nid)
{
	dn->inode = inode;
	dn->inode_block = hi;
	dn->node_block = db;
	dn->nid = nid;
}

/**
 * The maximum depth is 4.
 */
int get_node_path(long block, int offset[4], unsigned int noffset[4])
{
	const long direct_index = NORMAL_ADDRS_PER_INODE;
	const long direct_blks = ADDRS_PER_BLOCK;
	const long dptrs_per_blk = NIDS_PER_BLOCK;
	const long indirect_blks = ADDRS_PER_BLOCK * NIDS_PER_BLOCK;
	const long dindirect_blks = indirect_blks * NIDS_PER_BLOCK;
	int n = 0;
	int level = 0;

	noffset[0] = 0;

	if (block < direct_index) {
		offset[n] = block;
		goto got;
	}

	/* direct block 1 */
	block -= direct_index;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR1_BLOCK;
		noffset[n] = 1;
		offset[n] = block;
		level = 1;
		goto got;
	}

	/* direct block 2 */
	block -= direct_blks;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR2_BLOCK;
		noffset[n] = 2;
		offset[n] = block;
		level = 1;
		goto got;
	}

	/* indirect block 1 */
	block -= direct_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND1_BLOCK;
		noffset[n] = 3;
		offset[n++] = block / direct_blks;
		noffset[n] = 4 + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}

	/* indirect block 2 */
	block -= indirect_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND2_BLOCK;
		noffset[n] = 4 + dptrs_per_blk;
		offset[n++] = block / direct_blks;
		noffset[n] = 5 + dptrs_per_blk + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}

	/* double indirect block */
	block -= indirect_blks;
	if (block < dindirect_blks) {
		offset[n++] = NODE_DIND_BLOCK;
		noffset[n] = 5 + (dptrs_per_blk * 2);
		offset[n++] = block / indirect_blks;
		noffset[n] = 6 + (dptrs_per_blk * 2) +
		    offset[n - 1] * (dptrs_per_blk + 1);
		offset[n++] = (block / direct_blks) % dptrs_per_blk;
		noffset[n] = 7 + (dptrs_per_blk * 2) +
		    offset[n - 2] * (dptrs_per_blk + 1) + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 3;
		goto got;

	} else {
		BUG();
	}
got:
	return level;
}

static struct nat_entry *__lookup_nat_cache(struct hmfs_nm_info *nm_i, nid_t n)
{
	return radix_tree_lookup(&nm_i->nat_root, n);
}

static int init_node_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	struct hmfs_checkpoint *cp = ADDR(sbi, cp_i->load_checkpoint_addr);

	nm_i->max_nid = hmfs_max_nid();
	nm_i->nat_cnt = 0;
	nm_i->free_nids = kzalloc(HMFS_PAGE_SIZE * 2, GFP_KERNEL);
	nm_i->next_scan_nid = le64_to_cpu(cp->next_scan_nid);
	if (nm_i->free_nids == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&nm_i->nat_entries);
	INIT_LIST_HEAD(&nm_i->dirty_nat_entries);
	INIT_LIST_HEAD(&nm_i->free_nid_list);
	INIT_RADIX_TREE(&nm_i->nat_root, GFP_ATOMIC);
	rwlock_init(&nm_i->nat_tree_lock);
	spin_lock_init(&nm_i->free_nid_list_lock);
	mutex_init(&nm_i->build_lock);
	return 0;
}

void alloc_nid_failed(struct hmfs_sb_info *sbi, nid_t nid)
{
	struct hmfs_nm_info *nm_i = NM_I(sbi);

	mutex_lock(&nm_i->build_lock);
	spin_lock(&nm_i->free_nid_list_lock);
	/*
	 * here, we have lost free bit of nid, therefore, we set
	 * free bit of nid for every nid which is fail in
	 * allocation
	 */
	nm_i->free_nids[nm_i->fcnt].nid = make_free_nid(nid, 1);
	nm_i->fcnt++;
	spin_unlock(&nm_i->free_nid_list_lock);
	mutex_unlock(&nm_i->build_lock);
}

int build_node_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_nm_info *info;
	struct super_block *sb = sbi->sb;
	int err;

	info = kzalloc(sizeof(struct hmfs_nm_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	sbi->nm_info = info;

	err = init_node_manager(sbi);
	if (err) {
		goto free_nm;
	}

	info->nat_inode = hmfs_iget(sb, HMFS_NAT_INO);

	if (IS_ERR(info->nat_inode)) {
		err = PTR_ERR(info->nat_inode);
		goto free_nm;
	}

	return 0;
free_nm:
	kfree(info);
	return err;
}

static struct hmfs_nat_block *get_current_nat_block(struct hmfs_sb_info *sbi,
						    nid_t nid)
{
	//TODO:
	return NULL;
}

static struct nat_entry *grab_nat_entry(struct hmfs_nm_info *nm_i, nid_t nid)
{
	struct nat_entry *new;

	new = kmem_cache_alloc(nat_entry_slab, GFP_ATOMIC);
	if (!new)
		return NULL;
	if (radix_tree_insert(&nm_i->nat_root, nid, new)) {
		kmem_cache_free(nat_entry_slab, new);
		return NULL;
	}
	memset(new, 0, sizeof(struct nat_entry));
	write_lock(&nm_i->nat_tree_lock);
	new->ni.nid = nid;
	list_add_tail(&new->list, &nm_i->nat_entries);
	nm_i->nat_cnt++;
	write_unlock(&nm_i->nat_tree_lock);
	return new;
}

void truncate_node(struct dnode_of_data *dn)
{
	struct hmfs_sb_info *sbi = HMFS_SB(dn->inode->i_sb);
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct node_info ni;

	get_node_info(sbi, dn->nid, &ni);
	if (dn->inode->i_blocks == 0) {
		BUG_ON(ni.blk_addr != NULL_ADDR);
		goto invalidate;
	}

	BUG_ON(ni.blk_addr == NULL_ADDR);

	invalidate_block(sbi, ni.blk_addr);
	dec_valid_node_count(sbi, dn->inode, 1);
	update_nat_entry(nm_i, dn->nid, dn->inode->i_ino,
			 NULL_ADDR, CURCP_I(sbi)->store_version, true);

	/*
	 * ????
	 */
	if (dn->nid == dn->inode->i_ino) {
		remove_orphan_inode(sbi, dn->nid);
		dec_valid_inode_count(sbi);
	} else {
		mark_inode_dirty(dn->inode);
	}
invalidate:
	dn->node_block = NULL;
}

static int truncate_dnode(struct dnode_of_data *dn)
{
	struct hmfs_sb_info *sbi = HMFS_SB(dn->inode->i_sb);
	struct direct_node *hn;
	if (dn->nid == 0)
		return 1;

	hn = get_node(sbi, dn->nid);
	if (IS_ERR(hn) && PTR_ERR(hn) == -ENODATA)
		return 1;
	else if (IS_ERR(hn))
		return PTR_ERR(hn);

	dn->node_block = hn;
	dn->ofs_in_node = 0;
	truncate_data_blocks(dn);
	truncate_node(dn);
	return 1;
}

static int truncate_nodes(struct dnode_of_data *dn, unsigned int nofs, int ofs,
			  int depth)
{
	struct hmfs_sb_info *sbi = HMFS_SB(dn->inode->i_sb);
	struct dnode_of_data rdn;
	struct hmfs_node *hn;
	nid_t child_nid;
	unsigned int child_nofs;
	int freed = 0;
	int i, ret;

	if (dn->nid == 0)
		return NIDS_PER_BLOCK + 1;

	hn = get_new_node(sbi, dn->nid, dn->inode);
	if (IS_ERR(hn))
		return PTR_ERR(hn);

	if (depth < 3) {
		for (i = ofs; i < NIDS_PER_BLOCK; i++, freed++) {
			child_nid = le64_to_cpu(hn->in.nid[i]);
			if (child_nid == 0)
				continue;
			rdn.nid = child_nid;
			rdn.inode = dn->inode;
			ret = truncate_dnode(&rdn);
			if (ret < 0)
				goto out_err;
			set_nid(hn, i, 0, false);
		}
	} else {
		child_nofs = nofs + ofs * (NIDS_PER_BLOCK + 1) + 1;
		for (i = ofs; i < NIDS_PER_BLOCK; i++) {
			child_nid = le64_to_cpu(hn->in.nid[i]);
			if (child_nid == 0) {
				child_nofs += NIDS_PER_BLOCK + 1;
				continue;
			}
			rdn.nid = child_nid;
			rdn.inode = dn->inode;
			ret = truncate_nodes(&rdn, child_nofs, 0, depth - 1);
			if (ret == (NIDS_PER_BLOCK + 1)) {
				set_nid(hn, i, 0, false);
				child_nofs += ret;
			} else if (ret && ret != -ENODATA)
				goto out_err;
		}
		freed = child_nofs;
	}

	if (!ofs) {
		truncate_node(dn);
		freed++;
	}
	return freed;
out_err:
	return ret;
}

static int truncate_partial_nodes(struct dnode_of_data *dn,
				  struct hmfs_inode *hi, int *offset, int depth)
{
	struct hmfs_sb_info *sbi = HMFS_SB(dn->inode->i_sb);
	nid_t nid[3];
	struct hmfs_node *nodes[2];
	nid_t child_nid;
	int err = 0;
	int i;
	int idx = depth - 2;

	nid[0] = le64_to_cpu(hi->i_nid[offset[0] - NODE_DIR1_BLOCK]);
	if (!nid[0])
		return 0;

	/* get indirect nodes in the path */
	for (i = 0; i < depth - 1; i++) {
		nodes[i] = get_node(sbi, nid[i]);
		if (IS_ERR(nodes[i])) {
			depth = i + 1;
			err = PTR_ERR(nodes[i]);
			goto fail;
		}
		nid[i + 1] = get_nid(nodes[i], offset[i + 1], false);
	}

	/* free direct nodes linked to a partial indirect node */
	for (i = offset[depth - 1]; i < NIDS_PER_BLOCK; i++) {
		child_nid = get_nid(nodes[idx], i, false);
		if (!child_nid)
			continue;
		dn->nid = child_nid;
		err = truncate_dnode(dn);
		if (err < 0)
			goto fail;
		nodes[idx] = get_new_node(sbi, nid[idx], dn->inode);
		if (IS_ERR(nodes[idx])) {
			err = PTR_ERR(nodes[idx]);
			goto fail;
		}
		set_nid(nodes[idx], i, 0, false);
	}

	/* FIXME: should skip check in truncate_inode_blocks? */
	if (offset[depth - 1] == 0) {
		dn->nid = nid[idx];
		truncate_node(dn);
	}

	offset[idx]++;
	offset[depth - 1] = 0;
fail:
	return err;
}

int truncate_inode_blocks(struct inode *inode, pgoff_t from)
{
	struct hmfs_sb_info *sbi = HMFS_SB(inode->i_sb);
	int err = 0, cont = 1;
	int level, offset[4], noffset[4];
	unsigned int nofs = 0;
	struct hmfs_node *hn;
	struct dnode_of_data dn;

	level = get_node_path(from, offset, noffset);
	hn = get_node(sbi, inode->i_ino);
	if (IS_ERR(hn))
		return PTR_ERR(hn);

	set_new_dnode(&dn, inode, &hn->i, NULL, 0);
	switch (level) {
	case 0:
	case 1:
		nofs = noffset[1];
		break;
	case 2:
		nofs = noffset[1];
		if (!offset[level - 1])
			goto skip_partial;
		err = truncate_partial_nodes(&dn, &hn->i, offset, level);
		if (err < 0 && err != -ENODATA)
			goto fail;
		nofs += 1 + NIDS_PER_BLOCK;
		break;
	case 3:
		nofs = 5 + 2 * NIDS_PER_BLOCK;
		if (!offset[level - 1])
			goto skip_partial;
		err = truncate_partial_nodes(&dn, &hn->i, offset, level);
		if (err < 0 && err != -ENODATA)
			goto fail;
		break;
	default:
		BUG();
	}
skip_partial:
	while (cont) {
		dn.nid = le64_to_cpu(hn->i.i_nid[offset[0] - NODE_DIR1_BLOCK]);
		switch (offset[0]) {
		case NODE_DIR1_BLOCK:
		case NODE_DIR2_BLOCK:
			err = truncate_dnode(&dn);
			break;
		case NODE_IND1_BLOCK:
		case NODE_IND2_BLOCK:
			err = truncate_nodes(&dn, nofs, offset[1], 2);
			break;
		case NODE_DIND_BLOCK:
			err = truncate_nodes(&dn, nofs, offset[1], 3);
			cont = 0;
			break;
		default:
			BUG();
		}
		if (err < 0 && err != -ENODATA)
			goto fail;
		if (offset[1] == 0 && hn->i.i_nid[offset[0] - NODE_DIR1_BLOCK]) {
			hn = get_new_node(sbi, inode->i_ino, inode);
			if (IS_ERR(hn)) {
				err = PTR_ERR(hn);
				goto fail;
			}
			hn->i.i_nid[offset[0] - NODE_DIR1_BLOCK] = 0;
		}
		offset[1] = 0;
		offset[0]++;
		nofs += err;
	}
fail:
	return err > 0 ? 0 : err;
}

void update_nat_entry(struct hmfs_nm_info *nm_i, nid_t nid, nid_t ino,
		      unsigned long blk_addr, unsigned int version, bool dirty)
{
	struct nat_entry *e, *le;
retry:
	e = __lookup_nat_cache(nm_i, nid);
	if (!e) {
		e = grab_nat_entry(nm_i, nid);
		if (!e) {
			goto retry;
		}
	}
	write_lock(&nm_i->nat_tree_lock);
	e->ni.ino = ino;
	e->ni.nid = nid;
	e->ni.blk_addr = blk_addr;
	e->ni.version = version;
	if (dirty) {
		list_del(&e->list);
		le = list_entry(nm_i->dirty_nat_entries.next, struct nat_entry,
				list);
		if (e->ni.nid < le->ni.nid) {
			list_add_tail(&e->list, &le->list);
		} else {
			list_for_each_entry(le, &nm_i->dirty_nat_entries, list) {
				if (e->ni.nid > le->ni.nid) {
					break;
				}
			}
			list_add(&e->list, &le->list);
		}
	}
	write_unlock(&nm_i->nat_tree_lock);
}

static void _cache_nat_entry(struct hmfs_nm_info *nm_i, nid_t nid, nid_t ino,
			     unsigned long blk_addr, unsigned int version)
{
	struct nat_entry *e;
retry:
	e = __lookup_nat_cache(nm_i, nid);
	if (!e) {
		e = grab_nat_entry(nm_i, nid);
		if (!e) {
			goto retry;
		}
	}
	e->ni.ino = ino;
	e->ni.blk_addr = blk_addr;
	e->ni.version = version;
	printk(KERN_INFO "cache nat nid:%d ino:%d blk:%d-%d\n", nid, ino,
	       blk_addr >> HMFS_SEGMENT_SIZE_BITS,
	       (blk_addr & ~HMFS_SEGMENT_MASK) >> HMFS_PAGE_SIZE_BITS);

}

static inline unsigned long cal_page_addr(struct hmfs_sb_info *sbi,
					  u64 cur_node_blk_order,
					  u64 cur_node_blkoff)
{
	return (cur_node_blk_order << HMFS_SEGMENT_SIZE_BITS) +
	    (cur_node_blkoff << HMFS_PAGE_SIZE_BITS) + sbi->main_addr_start;
}

/*
 * return node address in NVM by nid, would not allocate
 * new node
 */
void *get_node(struct hmfs_sb_info *sbi, nid_t nid)
{
	struct node_info ni;
	int err;

	err = get_node_info(sbi, nid, &ni);
	if (err)
		return ERR_PTR(err);
	if (ni.blk_addr == NULL_ADDR)
		return ERR_PTR(-ENODATA);
	else if (ni.blk_addr == NEW_ADDR || ni.blk_addr == FREE_ADDR) {
		return ERR_PTR(-EINVAL);
	}
	return ADDR(sbi, ni.blk_addr);
}

void *get_new_node(struct hmfs_sb_info *sbi, nid_t nid, struct inode *inode)
{
	void *src;
	unsigned long block;
	struct hmfs_node *dest;
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	struct hmfs_summary *summary = NULL;
	src = get_node(sbi, nid);

	if (!IS_ERR(src)) {
		summary = get_summary_by_addr(sbi, src);
		if (get_summary_start(summary) == cp_i->store_version)
			return src;
	}

	if (!inc_valid_node_count(sbi, inode, 1))
		return ERR_PTR(-ENOSPC);

	if (is_inode_flag_set(HMFS_I(inode), FI_NO_ALLOC))
		return ERR_PTR(-EPERM);

	block = get_free_node_block(sbi);
	dest = ADDR(sbi, block);
	if (!IS_ERR(src)) {
		hmfs_memcpy(dest, src, HMFS_PAGE_SIZE);
	} else {
		memset_nt(dest, 0, HMFS_PAGE_SIZE - sizeof(struct node_footer));
		dest->footer.ino = cpu_to_le64(inode->i_ino);
		dest->footer.nid = cpu_to_le64(nid);
		dest->footer.cp_ver = cpu_to_le32(cp_i->store_version);
	}

	summary = get_summary_by_addr(sbi, dest);
//      FIXME: 'SUM_TYPE_INODE' here should be classified to SUM_TYPE_INODE or SUM_TYPE_IDN or SUM_TYPE_DN by given type
	make_summary_entry(summary, inode->i_ino, 0, cp_i->store_version, 1, 0,
			   SUM_TYPE_INODE);
	//TODO: cache nat
	update_nat_entry(nm_i, nid, inode->i_ino, block, cp_i->store_version,
			 true);
	return dest;
}

int get_node_info(struct hmfs_sb_info *sbi, nid_t nid, struct node_info *ni)
{
	struct checkpoint_info *cp_info = CURCP_I(sbi);
	struct hmfs_nat_entry ne;
	nid_t start_nid = START_NID(nid);
	struct nat_entry *e;
	struct hmfs_nat_block *nat_block;
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	int i;
	bool dirty;

	/* search in nat cache */
	e = __lookup_nat_cache(nm_i, nid);
	if (e) {
		read_lock(&nm_i->nat_tree_lock);
		ni->ino = e->ni.ino;
		ni->blk_addr = e->ni.blk_addr;
		ni->version = e->ni.version;
		read_unlock(&nm_i->nat_tree_lock);
		return 0;
	}

	/* search nat journals */
	i = lookup_journal_in_cp(cp_info, NAT_JOURNAL, nid, 0);
	if (i >= 0) {
		ne = nat_in_journal(cp_info, i);
		node_info_from_raw_nat(ni, &ne);
		dirty = true;
		goto cache;
	}

	/* search in main area */
	nat_block = get_current_nat_block(sbi, start_nid);
	if (nat_block == NULL)
		return -ENODATA;
	ne = nat_block->entries[nid - start_nid];
	node_info_from_raw_nat(ni, &ne);
	dirty = false;
cache:
	//TODO: add nat cache
	return 0;
}

static void add_free_nid(struct hmfs_nm_info *nm_i, nid_t nid, u64 free,
			 int *pos)
{
	spin_lock(&nm_i->free_nid_list_lock);
	nm_i->free_nids[*pos].nid = make_free_nid(nid, free);
	spin_unlock(&nm_i->free_nid_list_lock);
}

static void recycle_nat_journals(struct hmfs_sb_info *sbi,
				 struct hmfs_nm_info *nm_i, int *pos)
{
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	struct hmfs_checkpoint *hmfs_cp = cp_i->cp;
	int i;
	nid_t nid;
	u64 blk_addr;

	write_lock(&cp_i->journal_lock);
	for (i = 0; i < NUM_NAT_JOURNALS_IN_CP && *pos >= 0; ++i) {
		nid = le64_to_cpu(hmfs_cp->nat_journals[i].nid);
		blk_addr =
		    le64_to_cpu(hmfs_cp->nat_journals[i].entry.block_addr);
		if (blk_addr == FREE_ADDR && nid > HMFS_ROOT_INO) {
			hmfs_cp->nat_journals[i].nid = 0;
			add_free_nid(nm_i, nid, 1, pos);
			*pos = *pos - 1;
		}
	}
	write_unlock(&cp_i->journal_lock);
}

static nid_t scan_nat_block(struct hmfs_nm_info *nm_i,
			    struct hmfs_nat_block *nat_blk, nid_t start_nid,
			    int *pos)
{
	int i = start_nid % NAT_ENTRY_PER_BLOCK;
	u64 blk_addr;

	for (; i < NAT_ENTRY_PER_BLOCK && *pos >= 0; i++, start_nid++) {
		if (start_nid > nm_i->max_nid)
			break;

		if (nat_blk != NULL)
			blk_addr = le64_to_cpu(nat_blk->entries[i].block_addr);
		else
			goto found;

		if (blk_addr == FREE_ADDR) {
found:
			add_free_nid(nm_i, start_nid, 0, pos);
			*pos = *pos - 1;
		}
	}
	return start_nid;
}

static int build_free_nids(struct hmfs_sb_info *sbi)
{
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct hmfs_nat_block *nat_block = NULL;
	nid_t nid = nm_i->next_scan_nid;
	int pos = BUILD_FREE_NID_COUNT - 1;

	if (nm_i->fcnt >= BUILD_FREE_NID_COUNT)
		return nm_i->fcnt;

	BUG_ON(nm_i->fcnt != 0);

	recycle_nat_journals(sbi, nm_i, &pos);

	while (pos >= 0 && nid < nm_i->max_nid) {
		nat_block = get_current_nat_block(sbi, nid);
		nid = scan_nat_block(nm_i, nat_block, nid, &pos);
	}

	nm_i->next_scan_nid = nid;
	return BUILD_FREE_NID_COUNT - 1 - pos;
}

bool alloc_nid(struct hmfs_sb_info * sbi, nid_t * nid)
{
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	int num;

retry:
	if (cp_i->valid_node_count + 1 >= nm_i->max_nid)
		return false;

	spin_lock(&nm_i->free_nid_list_lock);

	if (nm_i->fcnt > 0) {
		*nid = get_free_nid(nm_i->free_nids[nm_i->fcnt - 1].nid);
		nm_i->fcnt--;
		spin_unlock(&nm_i->free_nid_list_lock);
		return true;
	}
	spin_unlock(&nm_i->free_nid_list_lock);

	//FIXME: Is there deadlock here?
	mutex_lock(&nm_i->build_lock);
	num = build_free_nids(sbi);
	spin_lock(&nm_i->free_nid_list_lock);
	nm_i->fcnt = num;
	spin_unlock(&nm_i->free_nid_list_lock);
	mutex_unlock(&nm_i->build_lock);
	goto retry;
}

void destroy_node_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_nm_info *info = NM_I(sbi);
	kfree(info->free_nids);
	iput(info->nat_inode);
	kfree(info);
}

int create_node_manager_caches(void)
{
	nat_entry_slab = hmfs_kmem_cache_create("nat_entry",
						sizeof(struct nat_entry), NULL);
	if (!nat_entry_slab)
		return -ENOMEM;

	return 0;
}

void destroy_node_manager_caches(void)
{
	kmem_cache_destroy(nat_entry_slab);
}

/* NAT index-tree on NVM */
inline void get_child_nat_addr(struct hmfs_sb_info *sbi,
			       struct hmfs_nat_node *cur_node,
			       unsigned int order,
			       struct hmfs_nat_node **next_node,
			       u32 * new_order, u8 height)
{
	if (!cur_node) {
		*next_node = NULL;
		return;
	}

	u16 _ofs;
	_ofs = (height - 1) * LOG2_NAT_ADDRS_PER_NODE;
	*next_node =
	    (struct hmfs_nat_node *)le64_to_cpu(cur_node->addr[order >> _ofs]);
	*new_order = order & ((1 << _ofs) - 1);
}

/* get a nat/nat page from nat/nat in-NVM tree */
static void *__get_nat_page(struct hmfs_sb_info *sbi,
			    unsigned long long cur_node_addr,
			    unsigned int order, u8 height)
{
	struct hmfs_nat_node *next_node, *cur_node;
	u32 new_order;

	if (!cur_node_addr) {
		//uninitialized
		return NULL;
	}
	cur_node = ADDR(sbi, cur_node_addr);
	if (!height)
		return (void *)cur_node;
	get_child_nat_addr(sbi, cur_node, order, &next_node, &new_order,
			   height);
	return __get_nat_page(sbi, next_node, new_order, height - 1);
}

void *get_nat_page(struct hmfs_sb_info *sbi, u32 nid)
{
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct checkpoint_info *cp_info = sbi->cp_info;

	unsigned int blk_order = nid >> LOG2_NAT_ENTRY_PER_BLOCK;
	return __get_nat_page(sbi, cp_info->cur_nat_root, blk_order,
			      raw_super->nat_height);
}

void *get_nat_page_by_order(struct hmfs_sb_info *sbi, unsigned int blk_order)
{
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct checkpoint_info *cp_info = sbi->cp_info;

	return __get_nat_page(sbi, cp_info->cur_nat_root, blk_order,
			      raw_super->nat_height);
}


static block_t recursive_flush_nat_pages(struct hmfs_sb_info *sbi,
					 struct hmfs_nat_node *old_nat_node,
					 struct hmfs_nat_node *cur_nat_node,
					 unsigned int blk_order, u8 height,
					 void *nat_entry_page,
					 unsigned short *alloc_cnt)
{
	//FIXME : cannot handle no space
	//TODO : add SSA support
	struct hmfs_nat_node *cur_stored_node, *old_child_node, *cur_child_node;
	block_t cur_stored_addr, child_stored_addr;
	unsigned int new_blk_order, _ofs;

	//leaf, alloc & copy nat info block 
	if (!height) {
		tprint("<%s:%d> leaf cur_nat_node:%p", __FUNCTION__, height,
		       cur_nat_node);
		cur_stored_addr = get_free_node_block(sbi);
		cur_stored_node = ADDR(sbi, cur_stored_addr);
		memcpy(cur_stored_node, nat_entry_page, HMFS_PAGE_SIZE);
		(*alloc_cnt) += 1;
		return cur_stored_addr;
	}

	cur_stored_node = cur_nat_node;
	cur_stored_addr = NULL;

	tprint("<%s:%d> old:%p, cur:%p", __FUNCTION__, height, old_nat_node,
	       cur_nat_node);
	if (cur_nat_node == NULL) {
		//only allocate new node_blk
		cur_stored_addr = get_free_node_block(sbi);
		cur_stored_node = ADDR(sbi, cur_stored_addr);
		memset_nt(cur_stored_node, 0, HMFS_PAGE_SIZE);
		(*alloc_cnt) += 1;
		tprint("<%s:%d> || new allocated addr:%p", __FUNCTION__, height,
		       cur_stored_node);
	} else if (old_nat_node == cur_nat_node) {
		//this node is not wandered before, COW
		cur_stored_addr = get_free_node_block(sbi);
		cur_stored_node = ADDR(sbi, cur_stored_addr);
		memcpy(cur_stored_node, old_nat_node, HMFS_PAGE_SIZE);
		(*alloc_cnt) += 1;
		tprint("<%s:%d> not wandered before || new allocated addr:%p",
		       __FUNCTION__, height, cur_stored_node);
	}
	//go to child
	get_child_nat_addr(sbi, old_nat_node, blk_order, &old_child_node,
			   &new_blk_order, height);
	if (old_child_node != NULL) {
		old_child_node = ADDR(sbi, (block_t) old_child_node);
	}
	get_child_nat_addr(sbi, cur_stored_node, blk_order, &cur_child_node,
			   &new_blk_order, height);
	tprint("<%s> old_child:%p, cur_child:%p", __FUNCTION__, old_child_node,
	       cur_child_node);
	if (cur_child_node != NULL) {
		cur_child_node = ADDR(sbi, (block_t) cur_child_node);
	}
	child_stored_addr =
	    recursive_flush_nat_pages(sbi, old_child_node, cur_child_node,
				      new_blk_order, height - 1, nat_entry_page,
				      alloc_cnt);
	if (child_stored_addr) {
		_ofs = blk_order >> ((height - 1) * LOG2_NAT_ADDRS_PER_NODE);
		cur_stored_node->addr[_ofs] = cpu_to_le64(child_stored_addr);
		tprint("<%s:%d> [[%p]] --> [[%p]]", __FUNCTION__, height,
		       cur_stored_node, ADDR(sbi, child_stored_addr));
	}
	return cur_stored_addr;
}

int do_flush_nat_page_test(struct hmfs_sb_info *sbi)
{
	//
	struct hmfs_super_block *raw_super;
	struct hmfs_checkpoint *hmfs_cp;
	struct hmfs_nat_node *old_root_node, *new_root_node;
	struct hmfs_nat_block *old_entry_block, *new_entry_block;
	block_t cp_addr, nat_addr, new_nat_root_addr;
	struct hmfs_nm_info *nm_i;
	struct nat_entry *ne;
	struct page *empty_page;
	unsigned short alloc_cnt;
	unsigned char nat_height;

	block_t old_blk_order, new_blk_order, _ofs;

	nm_i = NM_I(sbi);
	raw_super = HMFS_RAW_SUPER(sbi);
	cp_addr = le64_to_cpu(raw_super->cp_page_addr);
	hmfs_cp = ADDR(sbi, cp_addr);
	nat_addr = le64_to_cpu(hmfs_cp->nat_addr);

	empty_page = alloc_page(GFP_KERNEL);
	if (!empty_page) {
		return -ENOMEM;
	}
	new_entry_block = kmap(empty_page);	//FIXME: undo if failed

	nat_height = raw_super->nat_height;
	alloc_cnt = 0;
	new_root_node = old_root_node = ADDR(sbi, nat_addr);
	//init first page
	ne = list_entry(nm_i->dirty_nat_entries.next, struct nat_entry, list);
	old_blk_order = (ne->ni.nid) >> LOG2_NAT_ENTRY_PER_BLOCK;
	old_entry_block = get_nat_page(sbi, ne->ni.nid);
	memcpy(new_entry_block, old_entry_block, HMFS_PAGE_SIZE);

	/* FIXME :
	 * 1) no space
	 * 2) lock for dirty entry list
	 * 3) summary related work
	 */
	list_for_each_entry_from(ne, &nm_i->dirty_nat_entries, list) {
		new_blk_order = (ne->ni.nid) >> LOG2_NAT_ENTRY_PER_BLOCK;
		if (new_blk_order != old_blk_order) {
			// one page done, flush it
			new_nat_root_addr =
			    recursive_flush_nat_pages(sbi, old_root_node,
						      new_root_node,
						      old_blk_order, nat_height,
						      new_entry_block,
						      &alloc_cnt);
			if (new_nat_root_addr != NULL) {
				// root node not COWed
				new_root_node = ADDR(sbi, new_nat_root_addr);
			}
			old_blk_order = new_blk_order;
			old_entry_block = get_nat_page_by_order(sbi, old_blk_order);
			memcpy(new_entry_block, old_entry_block, HMFS_PAGE_SIZE);
		} else {
			//add a entry to a page
			_ofs = (ne->ni.nid) % LOG2_NAT_ENTRY_PER_BLOCK;
			node_info_to_raw_nat(&ne->ni,
					     &new_entry_block->entries[_ofs]);
		}
	}
/*
	new_nat_root_addr = recursive_flush_nat_pages(sbi, old_nat_node, cur_nat_node, 0, nat_height, nat_entry_page, &alloc_cnt);
	tprint("<%s> root_addr_1:%p, alloc_cnt:%d", __FUNCTION__, new_nat_root_addr, alloc_cnt);

	cur_nat_node = ADDR(sbi, new_nat_root_addr);
	new_nat_root_addr = recursive_flush_nat_pages(sbi, old_nat_node, cur_nat_node, 1, nat_height, nat_entry_page, &alloc_cnt);
	tprint("<%s> root_addr_2:%p, alloc_cnt:%d", __FUNCTION__, new_nat_root_addr, alloc_cnt);

	if(new_nat_root_addr!=NULL)
		cur_nat_node = ADDR(sbi, new_nat_root_addr);
	new_nat_root_addr = recursive_flush_nat_pages(sbi, old_nat_node, cur_nat_node, 513, nat_height, nat_entry_page, &alloc_cnt);
	tprint("<%s> root_addr_3:%p, alloc_cnt:%d", __FUNCTION__, new_nat_root_addr, alloc_cnt);

	if(new_nat_root_addr!=NULL)
		cur_nat_node = ADDR(sbi, new_nat_root_addr);
	new_nat_root_addr = recursive_flush_nat_pages(sbi, old_nat_node, cur_nat_node, 515, nat_height, nat_entry_page, &alloc_cnt);
	tprint("<%s> root_addr_4:%p, alloc_cnt:%d", __FUNCTION__, new_nat_root_addr, alloc_cnt);
	*/

	kunmap(empty_page);
	__free_page(empty_page);
	return 0;
}
