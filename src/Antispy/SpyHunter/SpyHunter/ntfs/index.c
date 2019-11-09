/* 
 * Copyright (c) [2010-2019] zhenfei.mzf@gmail.com rights reserved.
 * 
 * AntiSpy is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
*/
/**
 * index.c - NTFS index handling.  Originated from the Linux-NTFS project.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <windows.h>
#include <stddef.h>

#include "attrib.h"
#include "collate.h"
#include "debug.h"
#include "index.h"
#include "mst.h"
#include "dir.h"
#include "logging.h"
#include "bitmap.h"
#include "misc.h"

#pragma warning( disable: 4244 4761 )

//////////////////////////////////////////////////////////////////////////


/**
 * ntfs_index_entry_mark_dirty - mark an index entry dirty
 * @ictx:	ntfs index context describing the index entry
 *
 * Mark the index entry described by the index entry context @ictx dirty.
 *
 * If the index entry is in the index root attribute, simply mark the inode
 * containing the index root attribute dirty.  This ensures the mftrecord, and
 * hence the index root attribute, will be written out to disk later.
 *
 * If the index entry is in an index block belonging to the index allocation
 * attribute, set ib_dirty to TRUE, thus index block will be updated during
 * ntfs_index_ctx_put.
 */
void ntfs_index_entry_mark_dirty(ntfs_index_context *ictx)
{
	if (ictx->is_in_root)
		ntfs_inode_mark_dirty(ictx->actx->ntfs_ino);
	else
		ictx->ib_dirty = TRUE;
}

static s64 ntfs_ib_vcn_to_pos(ntfs_index_context *icx, VCN vcn)
{
	return vcn << icx->vcn_size_bits;
}

static VCN ntfs_ib_pos_to_vcn(ntfs_index_context *icx, s64 pos)
{
	return pos >> icx->vcn_size_bits;
}

static int ntfs_ib_write(ntfs_index_context *icx, VCN vcn, void *buf)
{
	s64 ret;
	
	ntfs_log_trace("vcn: %lld\n", vcn);
	
	ret = ntfs_attr_mst_pwrite(icx->ia_na, ntfs_ib_vcn_to_pos(icx, vcn),
				   1, icx->block_size, buf);
	if (ret != 1) {
		ntfs_log_perror("Failed to write index block %lld of inode "
				"%llu", (LONG64)vcn,
				(ULONG64)icx->ni->mft_no);
		return STATUS_ERROR;
	}
	
	return STATUS_OK;
}

static int ntfs_icx_ib_write(ntfs_index_context *icx)
{
		if (ntfs_ib_write(icx, icx->ib_vcn, icx->ib))
			return STATUS_ERROR;
		
		icx->ib_dirty = FALSE;
		
		return STATUS_OK;
}



ntfs_index_context *
ntfs_index_ctx_get(
	IN ntfs_inode *ni,
	IN ntfschar *name,
	IN u32 name_len
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/05 [5:3:2009 - 23:01]

Routine Description:
  ����&��ʼ�� һ�� ntfs_index_context �ṹ�� 

--*/
{
	ntfs_index_context *icx;

	ntfs_log_trace("Entering\n");
	
	if (!ni) {
		errno = EINVAL;
		return NULL;
	}
	if (ni->nr_extents == -1) { ni = ni->base_ni; }
	icx = ntfs_calloc(sizeof(ntfs_index_context));
	if (icx) 
	{
		icx->ni = ni;
		icx->name = name;
		icx->name_len = name_len;
	}

	return icx;
}



static void ntfs_index_ctx_free(ntfs_index_context *icx)
{
	ntfs_log_trace("Entering\n");
	
	if (!icx->entry)
		return;

	if (icx->actx)
		ntfs_attr_put_search_ctx(icx->actx);

	if (icx->is_in_root) {
		if (icx->ia_na)
			ntfs_attr_close(icx->ia_na);
		return;
	}

	if (icx->ib_dirty) {
		/* FIXME: Error handling!!! */
		ntfs_ib_write(icx, icx->ib_vcn, icx->ib);
	}
	
	free(icx->ib);
	ntfs_attr_close(icx->ia_na);
}

/**
 * ntfs_index_ctx_put - release an index context
 * @icx:	index context to free
 *
 * Release the index context @icx, releasing all associated resources.
 */
void ntfs_index_ctx_put(ntfs_index_context *icx)
{
	ntfs_index_ctx_free(icx);
	free(icx);
}

/**
 * ntfs_index_ctx_reinit - reinitialize an index context
 * @icx:	index context to reinitialize
 *
 * Reinitialize the index context @icx so it can be used for ntfs_index_lookup.
 */
void ntfs_index_ctx_reinit(ntfs_index_context *icx)
{
	ntfs_log_trace("Entering\n");
	
	ntfs_index_ctx_free(icx);

	icx->ni = icx->ni;
	icx->name = icx->name;
	icx->name_len = icx->name_len;
	
// 	*icx = (ntfs_index_context) {
// 		.ni = icx->ni,
// 		.name = icx->name,
// 		.name_len = icx->name_len,
// 	};

	return ;
}

static VCN *ntfs_ie_get_vcn_addr(INDEX_ENTRY *ie)
{
	return (VCN *)((u8 *)ie + le16_to_cpu(ie->length) - sizeof(VCN));
}

/**
 *  Get the subnode vcn to which the index entry refers.
 */
VCN ntfs_ie_get_vcn(INDEX_ENTRY *ie)
{
	return sle64_to_cpup(ntfs_ie_get_vcn_addr(ie));
}

static INDEX_ENTRY *ntfs_ie_get_first(INDEX_HEADER *ih)
{
	return (INDEX_ENTRY *)((u8 *)ih + le32_to_cpu(ih->entries_offset));
}

static INDEX_ENTRY *ntfs_ie_get_next(INDEX_ENTRY *ie)
{
	return (INDEX_ENTRY *)((char *)ie + le16_to_cpu(ie->length));
}

static u8 *ntfs_ie_get_end(INDEX_HEADER *ih)
{
	/* FIXME: check if it isn't overflowing the index block size */
	return (u8 *)ih + le32_to_cpu(ih->index_length);
}

static int ntfs_ie_end(INDEX_ENTRY *ie)
{
	return ie->ie_flags & INDEX_ENTRY_END;
}

/** 
 *  Find the last entry in the index block
 */
static INDEX_ENTRY *ntfs_ie_get_last(INDEX_ENTRY *ie, char *ies_end)
{
	ntfs_log_trace("Entering\n");
	
	while ((char *)ie < ies_end && !ntfs_ie_end(ie))
		ie = ntfs_ie_get_next(ie);
	
	return ie;
}

static INDEX_ENTRY *ntfs_ie_get_by_pos(INDEX_HEADER *ih, int pos)
{
	INDEX_ENTRY *ie;
	
	ntfs_log_trace("pos: %d\n", pos);
	
	ie = ntfs_ie_get_first(ih);
	
	while (pos-- > 0)
		ie = ntfs_ie_get_next(ie);
	
	return ie;
}

static INDEX_ENTRY *ntfs_ie_prev(INDEX_HEADER *ih, INDEX_ENTRY *ie)
{
	INDEX_ENTRY *ie_prev = NULL;
	INDEX_ENTRY *tmp;
	
	ntfs_log_trace("Entering\n");
	
	tmp = ntfs_ie_get_first(ih);
	
	while (tmp != ie) {
		ie_prev = tmp;
		tmp = ntfs_ie_get_next(tmp);
	}
	
	return ie_prev;
}

char *ntfs_ie_filename_get(INDEX_ENTRY *ie)
{
	FILE_NAME_ATTR *fn;

	fn = (FILE_NAME_ATTR *)&ie->key;
	return ntfs_attr_name_get(fn->file_name, fn->file_name_length);
}

void ntfs_ie_filename_dump(INDEX_ENTRY *ie)
{
	char *s;

	s = ntfs_ie_filename_get(ie);
	ntfs_log_debug("'%s' ", s);
	ntfs_attr_name_free(&s);
}

void ntfs_ih_filename_dump(INDEX_HEADER *ih)
{
	INDEX_ENTRY *ie;
	
	ntfs_log_trace("Entering\n");
	
	ie = ntfs_ie_get_first(ih);
	while (!ntfs_ie_end(ie)) {
		ntfs_ie_filename_dump(ie);
		ie = ntfs_ie_get_next(ie);
	}
}

static int ntfs_ih_numof_entries(INDEX_HEADER *ih)
{
	int n;
	INDEX_ENTRY *ie;
	
	ntfs_log_trace("Entering\n");
	
	ie = ntfs_ie_get_first(ih);
	for (n = 0; !ntfs_ie_end(ie); n++)
		ie = ntfs_ie_get_next(ie);
	return n;
}

static int ntfs_ih_one_entry(INDEX_HEADER *ih)
{
	return (ntfs_ih_numof_entries(ih) == 1);
}

static int ntfs_ih_zero_entry(INDEX_HEADER *ih)
{
	return (ntfs_ih_numof_entries(ih) == 0);
}

static void ntfs_ie_delete(INDEX_HEADER *ih, INDEX_ENTRY *ie)
{
	u32 new_size;
	
	ntfs_log_trace("Entering\n");
	
	new_size = le32_to_cpu(ih->index_length) - le16_to_cpu(ie->length);
	ih->index_length = cpu_to_le32(new_size);
	memmove(ie, (u8 *)ie + le16_to_cpu(ie->length),
		new_size - ((u8 *)ie - (u8 *)ih));
}

static void ntfs_ie_set_vcn(INDEX_ENTRY *ie, VCN vcn)
{
	*ntfs_ie_get_vcn_addr(ie) = cpu_to_le64(vcn);
}

/**
 *  Insert @ie index entry at @pos entry. Used @ih values should be ok already.
 */
static void ntfs_ie_insert(INDEX_HEADER *ih, INDEX_ENTRY *ie, INDEX_ENTRY *pos)
{
	int ie_size = le16_to_cpu(ie->length);
	
	ntfs_log_trace("Entering\n");
	
	ih->index_length = cpu_to_le32(le32_to_cpu(ih->index_length) + ie_size);
	memmove((u8 *)pos + ie_size, pos,
		le32_to_cpu(ih->index_length) - ((u8 *)pos - (u8 *)ih) - ie_size);
	memcpy(pos, ie, ie_size);
}

static INDEX_ENTRY *ntfs_ie_dup(INDEX_ENTRY *ie)
{
	INDEX_ENTRY *dup;
	
	ntfs_log_trace("Entering\n");
	
	dup = (INDEX_ENTRY *) ntfs_malloc(le16_to_cpu(ie->length));
	if (dup)
		memcpy(dup, ie, le16_to_cpu(ie->length));
	
	return dup;
}

static INDEX_ENTRY *ntfs_ie_dup_novcn(INDEX_ENTRY *ie)
{
	INDEX_ENTRY *dup;
	int size = le16_to_cpu(ie->length);
	
	ntfs_log_trace("Entering\n");
	
	if (ie->ie_flags & INDEX_ENTRY_NODE)
		size -= sizeof(VCN);
	
	dup = (INDEX_ENTRY *)ntfs_malloc(size);
	if (dup) {
		memcpy(dup, ie, size);
		dup->ie_flags &= ~INDEX_ENTRY_NODE;
		dup->length = cpu_to_le16(size);
	}
	return dup;
}

static int ntfs_ia_check(ntfs_index_context *icx, INDEX_BLOCK *ib, VCN vcn)
{
	u32 ib_size = (unsigned)le32_to_cpu(ib->index.allocated_size) + 0x18;
	
	ntfs_log_trace("Entering\n");
	
	if (!ntfs_is_indx_record(ib->magic)) {
		
		return -1;
	}
	
	if (sle64_to_cpu(ib->index_block_vcn) != vcn) {
		
		return -1;
	}
	
	if (ib_size != icx->block_size) {
		
		ntfs_log_error("Corrupt index block : VCN (%lld) of inode %llu "
			       "has a size (%u) differing from the index "
			       "specified size (%u)\n", (LONG64)vcn, 
			       (ULONG64)icx->ni->mft_no, ib_size,
			       icx->block_size);
		return -1;
	}
	return 0;
}



static INDEX_ROOT *
ntfs_ir_lookup(
	IN ntfs_inode *ni, 
	IN ntfschar *name,
	IN u32 name_len, 
	OUT ntfs_attr_search_ctx **ctx
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/05 [5:3:2009 - 23:09]

Routine Description:
  ����inode�õ�90H(INDEX_ROOT)����

--*/
{
	ATTR_RECORD *a;
	INDEX_ROOT *ir = NULL;

	ntfs_log_trace("Entering\n");
	
	*ctx = ntfs_attr_get_search_ctx( ni, NULL );
	if (!*ctx) {
		ntfs_log_perror("Failed to get $INDEX_ROOT search context");
		return NULL;
	}
	
	if ( ntfs_attr_lookup(AT_INDEX_ROOT, name, name_len, CASE_SENSITIVE, 0, NULL, 0, *ctx) ) 
	{
		ntfs_log_perror("Failed to lookup $INDEX_ROOT");
		goto err_out;
	}
	
	a = (*ctx)->attr;
	if (a->non_resident) {
		errno = EINVAL;
		ntfs_log_perror("Non-resident $INDEX_ROOT detected");
		goto err_out;
	}
	
	ir = (INDEX_ROOT *)((char *)a + le16_to_cpu(a->X.value_offset));

err_out:
	if (!ir) { ntfs_attr_put_search_ctx(*ctx); }
	return ir;
}

/** 
 * 
 * 
 * Return values:
 *   
 */
static int 
ntfs_ie_lookup(
	IN const void *key, 
	IN const int key_len,
	IN ntfs_index_context *icx, 
	IN INDEX_HEADER *ih,
	IN VCN *vcn, 
	OUT INDEX_ENTRY **ie_out
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/05 [5:3:2009 - 23:16]

Routine Description:
  Find a key in the index block.  
    
Arguments:

     - 

Return Value:
  STATUS_OK with errno set to ESUCCESS if we know for sure that the entry
    exists and @ie_out points to this entry.
  STATUS_NOT_FOUND with errno set to ENOENT if we know for sure the entry
    doesn't exist and @ie_out is the insertion point.
  STATUS_KEEP_SEARCHING if we can't answer the above question and @vcn will
    contain the node index block.
  STATUS_ERROR with errno set if on unexpected error during lookup.
    
--*/
{
	INDEX_ENTRY *ie;
	u8 *index_end;
	int rc, item = 0;
	 
	ntfs_log_trace("Entering\n");
	
	index_end = ntfs_ie_get_end(ih);

	for (ie = ntfs_ie_get_first(ih); ; ie = ntfs_ie_get_next(ie)) 
	{
		if ((u8 *)ie + sizeof(INDEX_ENTRY_HEADER) > index_end 
			|| (u8 *)ie + le16_to_cpu(ie->length) > index_end
		   ) 
		{
			errno = ERANGE;
			ntfs_log_error("Index entry out of bounds in inode %llu.\n", (ULONG64)icx->ni->mft_no);
			return STATUS_ERROR;
		}

		if ( ntfs_ie_end(ie) ) { break; }

		rc = ntfs_collate( icx->ni->vol, icx->cr, key, key_len, &ie->key, le16_to_cpu(ie->key_length) );
		if ( rc == NTFS_COLLATION_ERROR ) 
		{
			ntfs_log_error("Collation error. Perhaps a filename contains invalid characters?\n");
			errno = ERANGE;
			return STATUS_ERROR;
		}

		if ( rc == -1 ) { break; }
		if ( !rc )
		{
			*ie_out = ie;
			errno = 0;
			icx->parent_pos[icx->pindex] = item;
			return STATUS_OK;
		}
		
		item++;
	}

	/*
	 * We have finished with this index block without success. Check for the
	 * presence of a child node and if not present return with errno ENOENT,
	 * otherwise we will keep searching in another index block.
	 */
	if (!(ie->ie_flags & INDEX_ENTRY_NODE)) {
		ntfs_log_debug("Index entry wasn't found.\n");
		*ie_out = ie;
		errno = ENOENT;
		return STATUS_NOT_FOUND;
	}
	
	/* Get the starting vcn of the index_block holding the child node. */
	*vcn = ntfs_ie_get_vcn(ie);
	if (*vcn < 0) {
		errno = EINVAL;
		ntfs_log_perror("Negative vcn in inode %llu\n",
			       	(ULONG64)icx->ni->mft_no);
		return STATUS_ERROR;
	}

	ntfs_log_trace("Parent entry number %d\n", item);
	icx->parent_pos[icx->pindex] = item;
	
	return STATUS_KEEP_SEARCHING;
}



static ntfs_attr *ntfs_ia_open(ntfs_index_context *icx, ntfs_inode *ni)
{
	ntfs_attr *na;
	
	na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, icx->name, icx->name_len);
	if (!na) {
		ntfs_log_perror("Failed to open index allocation of inode %llu", (ULONG64)ni->mft_no);
		return NULL;
	}
	
	return na;
}

static int ntfs_ib_read(ntfs_index_context *icx, VCN vcn, INDEX_BLOCK *dst)
{
	s64 pos, ret;

	ntfs_log_trace("vcn: %lld\n", vcn);
	
	pos = ntfs_ib_vcn_to_pos(icx, vcn);

	ret = ntfs_attr_mst_pread(icx->ia_na, pos, 1, icx->block_size, (u8 *)dst);
	if (ret != 1) {
		if (ret == -1)
			ntfs_log_perror("Failed to read index block");
		else 
			ntfs_log_error("Failed to read full index block at %lld\n", (LONG64)pos);
		return -1;
	}
	
	if (ntfs_ia_check(icx, dst, vcn))
		return -1;
	
	return 0;
}

static int ntfs_icx_parent_inc(ntfs_index_context *icx)
{
	icx->pindex++;
	if (icx->pindex >= MAX_PARENT_VCN) {
//		errno = EOPNOTSUPP;
		ntfs_log_perror("Index is over %d level deep", MAX_PARENT_VCN);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int ntfs_icx_parent_dec(ntfs_index_context *icx)
{
	icx->pindex--;
	if (icx->pindex < 0) {
		errno = EINVAL;
		ntfs_log_perror("Corrupt index pointer (%d)", icx->pindex);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}
	


int 
ntfs_index_lookup(
	IN const void *key, 
	IN const int key_len,
	IN ntfs_index_context *icx
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/05 [5:3:2009 - 23:05]

Routine Description:
  find a key in an index and return its index entry  

Return Value:
  If the @key is found in the index, 0 is returned and @icx is setup to
  describe the index entry containing the matching @key.  @icx->entry is the
  index entry and @icx->data and @icx->data_len are the index entry data and
  its length in bytes, respectively.
 
  If the @key is not found in the index, -1 is returned, errno = ENOENT and
  @icx is setup to describe the index entry whose key collates immediately
  after the search @key, i.e. this is the position in the index at which
  an index entry with a key of @key would need to be inserted.

  If the index entry was modified, call ntfs_index_entry_mark_dirty() before
  the call to ntfs_index_ctx_put() to ensure that the changes are written to disk.
    
--*/
{
	VCN old_vcn, vcn;
	ntfs_inode *ni = icx->ni;
	INDEX_ROOT *ir;
	INDEX_ENTRY *ie;
	INDEX_BLOCK *ib = NULL;
	ntfs_attr_search_ctx *actx;
	int ret, err = 0;

	ntfs_log_trace("Entering\n");
	
	if (!key || key_len <= 0) {
		errno = EINVAL;
		ntfs_log_perror("key: %p  key_len: %d", key, key_len);
		return -1;
	}

	ir = ntfs_ir_lookup( ni, icx->name, icx->name_len, &actx );
	if (!ir) {
		if (errno == ENOENT)
			errno = EIO;
		return -1;
	}
	
	icx->block_size = le32_to_cpu(ir->index_block_size);
	if (icx->block_size < NTFS_BLOCK_SIZE) {
		errno = EINVAL;
		ntfs_log_perror("Index block size (%d) is smaller than the sector size (%d)", icx->block_size, NTFS_BLOCK_SIZE);
		return -1;
	}

	if (ni->vol->cluster_size <= icx->block_size)
		icx->vcn_size_bits = ni->vol->cluster_size_bits;
	else
		icx->vcn_size_bits = ni->vol->sector_size_bits;
		
	icx->cr = ir->collation_rule;
	if ( !ntfs_is_collation_rule_supported(icx->cr) ) {
		err = errno = /*EOPNOTSUPP*/1045;
		goto err_out;
	}
	
	old_vcn = VCN_INDEX_ROOT_PARENT;

	ret = ntfs_ie_lookup( key, key_len, icx, &ir->index, &vcn, &ie );
	if (ret == STATUS_ERROR) {
		err = errno;
		goto err_out;
	}
	
	icx->actx = actx;
	icx->ir = ir;
	
	if (ret != STATUS_KEEP_SEARCHING) {
		/* STATUS_OK or STATUS_NOT_FOUND */
		err = errno;
		icx->is_in_root = TRUE;
		icx->parent_vcn[icx->pindex] = old_vcn;
		goto done;
	}
	
	/* Child node present, descend into it. */
	
	icx->ia_na = ntfs_ia_open(icx, ni);
	if (!icx->ia_na)
		goto err_out;
	
	ib = (INDEX_BLOCK *) ntfs_malloc(icx->block_size);
	if (!ib) {
		err = errno;
		goto err_out;
	}
	
descend_into_child_node:

	icx->parent_vcn[icx->pindex] = old_vcn;
	if (ntfs_icx_parent_inc(icx)) {
		err = errno;
		goto err_out;
	}
	old_vcn = vcn;

	ntfs_log_debug("Descend into node with VCN %lld.\n", vcn);
	
	if (ntfs_ib_read(icx, vcn, ib)) { goto err_out; }
	
	ret = ntfs_ie_lookup( key, key_len, icx, &ib->index, &vcn, &ie );
	if ( ret != STATUS_KEEP_SEARCHING ) 
	{
		err = errno;
		if (ret == STATUS_ERROR)
			goto err_out;
		
		/* STATUS_OK or STATUS_NOT_FOUND */
		icx->is_in_root = FALSE;
		icx->ib = ib;
		icx->parent_vcn[icx->pindex] = icx->ib_vcn = vcn;
		goto done;
	}

	if ((ib->index.ih_flags & NODE_MASK) == LEAF_NODE) {
		ntfs_log_error("Index entry with child node found in a leaf node in inode 0x%llx.\n", (ULONG64)ni->mft_no);
		goto err_out;
	}
	
	goto descend_into_child_node;

err_out:
	if (icx->ia_na) { ntfs_attr_close(icx->ia_na); }
	free(ib);
	if (!err)
		err = EIO;
	if (actx)
		ntfs_attr_put_search_ctx(actx);
	errno = err;
	return -1;
done:
	icx->entry = ie;
	icx->data = (u8 *)ie + offsetof(INDEX_ENTRY, key);
	icx->data_len = le16_to_cpu(ie->key_length);
	icx->max_depth = icx->pindex;
	ntfs_log_trace("Done.\n");
	if (err) {
		errno = err;
		return -1;
	}

	return 0;
}



static INDEX_BLOCK *ntfs_ib_alloc(VCN ib_vcn, u32 ib_size, 
				  INDEX_HEADER_FLAGS node_type)
{
	INDEX_BLOCK *ib;
	int ih_size = sizeof(INDEX_HEADER);
	
	ntfs_log_trace("Entering ib_vcn = %lld ib_size = %u\n", ib_vcn, ib_size);
	
	ib = ntfs_calloc(ib_size);
	if (!ib)
		return NULL;
	
	ib->magic = magic_INDX;
	ib->usa_ofs = cpu_to_le16(sizeof(INDEX_BLOCK));
	ib->usa_count = cpu_to_le16(ib_size / NTFS_BLOCK_SIZE + 1);
	/* Set USN to 1 */
	*(u16 *)((char *)ib + le16_to_cpu(ib->usa_ofs)) = cpu_to_le16(1);
	ib->lsn = cpu_to_le64(0);
	
	ib->index_block_vcn = cpu_to_sle64(ib_vcn);
	
	ib->index.entries_offset = cpu_to_le32((ih_size +
			le16_to_cpu(ib->usa_count) * 2 + 7) & ~7);
	ib->index.index_length = 0;
	ib->index.allocated_size = cpu_to_le32(ib_size - (sizeof(INDEX_BLOCK) - ih_size));
	ib->index.ih_flags = node_type;
	
	return ib;
}	

/** 
 *  Find the median by going through all the entries
 */
static INDEX_ENTRY *ntfs_ie_get_median(INDEX_HEADER *ih)
{
	INDEX_ENTRY *ie, *ie_start;
	u8 *ie_end;
	int i = 0, median;
	
	ntfs_log_trace("Entering\n");
	
	ie = ie_start = ntfs_ie_get_first(ih);
	ie_end   = (u8 *)ntfs_ie_get_end(ih);
	
	while ((u8 *)ie < ie_end && !ntfs_ie_end(ie)) {
		ie = ntfs_ie_get_next(ie);
		i++;
	}
	/*
	 * NOTE: this could be also the entry at the half of the index block.
	 */
	median = i / 2 - 1;
	
	ntfs_log_trace("Entries: %d  median: %d\n", i, median);
	
	for (i = 0, ie = ie_start; i <= median; i++)
		ie = ntfs_ie_get_next(ie);
	
	return ie;
}

static s64 ntfs_ibm_vcn_to_pos(ntfs_index_context *icx, VCN vcn)
{
	return ntfs_ib_vcn_to_pos(icx, vcn) / icx->block_size;
}

static s64 ntfs_ibm_pos_to_vcn(ntfs_index_context *icx, s64 pos)
{
	return ntfs_ib_pos_to_vcn(icx, pos * icx->block_size);
}

static int ntfs_ibm_add(ntfs_index_context *icx)
{
	u8 bmp[8];

	ntfs_log_trace("Entering\n");
	
	if (ntfs_attr_exist(icx->ni, AT_BITMAP, icx->name, icx->name_len))
		return STATUS_OK;
	/*
	 * AT_BITMAP must be at least 8 bytes.
	 */
	memset(bmp, 0, sizeof(bmp));
	if (ntfs_attr_add(icx->ni, AT_BITMAP, icx->name, icx->name_len,bmp, sizeof(bmp))) 
	{
		ntfs_log_perror("Failed to add AT_BITMAP");
		return STATUS_ERROR;
	}
	
	return STATUS_OK;
}

static int ntfs_ibm_modify(ntfs_index_context *icx, VCN vcn, int set)
{
	u8 byte;
	s64 pos = ntfs_ibm_vcn_to_pos(icx, vcn);
	u32 bpos = pos / 8;
	u32 bit = 1 << (pos % 8);
	ntfs_attr *na;
	int ret = STATUS_ERROR;

	ntfs_log_trace("%s vcn: %lld\n", set ? "set" : "clear", vcn);
	
	na = ntfs_attr_open(icx->ni, AT_BITMAP,  icx->name, icx->name_len);
	if (!na) {
		ntfs_log_perror("Failed to open $BITMAP attribute");
		return -1;
	}

	if (set) 
	{
		if (na->data_size < bpos + 1) 
		{
			if (ntfs_attr_truncate(na, (na->data_size + 8) & ~7)) 
			{
				ntfs_log_perror("Failed to truncate AT_BITMAP");
				goto err_na;
			}
		}
	}
	
	if (ntfs_attr_pread(na, bpos, 1, &byte) != 1) {
		ntfs_log_perror("Failed to read $BITMAP");
		goto err_na;
	}

	if (set) 
		byte |= bit;
	else
		byte &= ~bit;
		
	if (ntfs_attr_pwrite(na, bpos, 1, &byte) != 1) {
		ntfs_log_perror("Failed to write $Bitmap");
		goto err_na;
	}

	ret = STATUS_OK;
err_na:
	ntfs_attr_close(na);
	return ret;
}


static int ntfs_ibm_set(ntfs_index_context *icx, VCN vcn)
{
	return ntfs_ibm_modify(icx, vcn, 1);
}

static int ntfs_ibm_clear(ntfs_index_context *icx, VCN vcn)
{
	return ntfs_ibm_modify(icx, vcn, 0);
}

static VCN ntfs_ibm_get_free(ntfs_index_context *icx)
{
	u8 *bm;
	int bit;
	s64 vcn, byte, size;

	ntfs_log_trace("Entering\n");
	
	bm = ntfs_attr_readall(icx->ni, AT_BITMAP,  icx->name, icx->name_len,
			       &size);
	if (!bm)
		return (VCN)-1;
	
	for (byte = 0; byte < size; byte++)
	{
		
		if (bm[byte] == 255)
			continue;
		
		for (bit = 0; bit < 8; bit++) 
		{
			if (!(bm[byte] & (1 << bit))) 
			{
				vcn = ntfs_ibm_pos_to_vcn(icx, byte * 8 + bit);
				goto out;
			}
		}
	}
	
	vcn = ntfs_ibm_pos_to_vcn(icx, size * 8);
out:	
	ntfs_log_trace("allocated vcn: %lld\n", vcn);

	if (ntfs_ibm_set(icx, vcn))
		vcn = (VCN)-1;
	
	free(bm);
	return vcn;
}

static INDEX_BLOCK *ntfs_ir_to_ib(INDEX_ROOT *ir, VCN ib_vcn)
{
	INDEX_BLOCK *ib;
	INDEX_ENTRY *ie_last;
	char *ies_start, *ies_end;
	int i;
	
	ntfs_log_trace("Entering\n");
	
	ib = ntfs_ib_alloc(ib_vcn, le32_to_cpu(ir->index_block_size), LEAF_NODE);
	if (!ib)
		return NULL;
	
	ies_start = (char *)ntfs_ie_get_first(&ir->index);
	ies_end   = (char *)ntfs_ie_get_end(&ir->index);
	ie_last   = ntfs_ie_get_last((INDEX_ENTRY *)ies_start, ies_end);
	/* 
	 * Copy all entries, including the termination entry
	 * as well, which can never have any data.
	 */
	i = (char *)ie_last - ies_start + le16_to_cpu(ie_last->length);
	memcpy(ntfs_ie_get_first(&ib->index), ies_start, i);
	
	ib->index.ih_flags = ir->index.ih_flags;
	ib->index.index_length = cpu_to_le32(i +
			le32_to_cpu(ib->index.entries_offset));
	return ib;
}

static void ntfs_ir_nill(INDEX_ROOT *ir)
{
	INDEX_ENTRY *ie_last;
	char *ies_start, *ies_end;
	
	ntfs_log_trace("Entering\n");
	/*
	 * TODO: This function could be much simpler.
	 */
	ies_start = (char *)ntfs_ie_get_first(&ir->index);
	ies_end   = (char *)ntfs_ie_get_end(&ir->index);
	ie_last   = ntfs_ie_get_last((INDEX_ENTRY *)ies_start, ies_end);
	/* 
	 * Move the index root termination entry forward
	 */
	if ((char *)ie_last > ies_start) {
		memmove(ies_start, (char *)ie_last, le16_to_cpu(ie_last->length));
		ie_last = (INDEX_ENTRY *)ies_start;
	}
}

static int ntfs_ib_copy_tail(ntfs_index_context *icx, INDEX_BLOCK *src,
			     INDEX_ENTRY *median, VCN new_vcn)
{
	u8 *ies_end;
	INDEX_ENTRY *ie_head;		/* first entry after the median */
	int tail_size, ret;
	INDEX_BLOCK *dst;
	
	ntfs_log_trace("Entering\n");
	
	dst = ntfs_ib_alloc(new_vcn, icx->block_size, 
			    src->index.ih_flags & NODE_MASK);
	if (!dst)
		return STATUS_ERROR;
	
	ie_head = ntfs_ie_get_next(median);
	
	ies_end = (u8 *)ntfs_ie_get_end(&src->index);
	tail_size = ies_end - (u8 *)ie_head;
	memcpy(ntfs_ie_get_first(&dst->index), ie_head, tail_size);
	
	dst->index.index_length = cpu_to_le32(tail_size + 
					      le32_to_cpu(dst->index.entries_offset));
	ret = ntfs_ib_write(icx, new_vcn, dst);

	free(dst);
	return ret;
}

static int ntfs_ib_cut_tail(ntfs_index_context *icx, INDEX_BLOCK *src,
			    INDEX_ENTRY *ie)
{
	char *ies_start, *ies_end;
	INDEX_ENTRY *ie_last;
	
	ntfs_log_trace("Entering\n");
	
	ies_start = (char *)ntfs_ie_get_first(&src->index);
	ies_end   = (char *)ntfs_ie_get_end(&src->index);
	
	ie_last   = ntfs_ie_get_last((INDEX_ENTRY *)ies_start, ies_end);
	if (ie_last->ie_flags & INDEX_ENTRY_NODE)
		ntfs_ie_set_vcn(ie_last, ntfs_ie_get_vcn(ie));
	
	memcpy(ie, ie_last, le16_to_cpu(ie_last->length));
	
	src->index.index_length = cpu_to_le32(((char *)ie - ies_start) + 
		le16_to_cpu(ie->length) + le32_to_cpu(src->index.entries_offset));
	
	if (ntfs_ib_write(icx, icx->parent_vcn[icx->pindex + 1], src))
		return STATUS_ERROR;
	
	return STATUS_OK;
}
	
static int ntfs_ia_add(ntfs_index_context *icx)
{
	ntfs_log_trace("Entering\n");

	if (ntfs_ibm_add(icx))
		return -1;
	
	if (!ntfs_attr_exist(icx->ni, AT_INDEX_ALLOCATION, icx->name, icx->name_len)) {
	
		if (ntfs_attr_add(icx->ni, AT_INDEX_ALLOCATION, icx->name, (u8)icx->name_len, NULL, 0)) 
		{
			ntfs_log_perror("Failed to add AT_INDEX_ALLOCATION");
			return -1;
		}
	}
	
	icx->ia_na = ntfs_ia_open(icx, icx->ni);
	if (!icx->ia_na)
		return -1;

	return 0;
}



static int ntfs_ir_reparent(ntfs_index_context *icx)
{
	ntfs_attr_search_ctx *ctx;
	INDEX_ROOT *ir;
	INDEX_ENTRY *ie;
	INDEX_BLOCK *ib = NULL;
	VCN new_ib_vcn;
	int ret = STATUS_ERROR;

	ntfs_log_trace("Entering\n");
	
	ir = ntfs_ir_lookup(icx->ni, icx->name, icx->name_len, &ctx);
	if (!ir)
		return -1;
	
	if ((ir->index.ih_flags & NODE_MASK) == SMALL_INDEX)
		if (ntfs_ia_add(icx))
			goto err_out;
	
	new_ib_vcn = ntfs_ibm_get_free(icx);
	if (new_ib_vcn == -1)
		goto err_out;
		
	ib = ntfs_ir_to_ib(ir, new_ib_vcn);
	if (ib == NULL) {
		ntfs_log_perror("Failed to move index root to index block");
		goto clear_bmp;
	}
		
	if (ntfs_ib_write(icx, new_ib_vcn, ib))
		goto clear_bmp;
	
	ntfs_ir_nill(ir);
	
	ie = ntfs_ie_get_first(&ir->index);
	ie->ie_flags |= INDEX_ENTRY_NODE;
	ie->length = cpu_to_le16(sizeof(INDEX_ENTRY_HEADER) + sizeof(VCN));
	
	ir->index.ih_flags = LARGE_INDEX;
	ir->index.index_length = cpu_to_le32(le32_to_cpu(ir->index.entries_offset)
					     + le16_to_cpu(ie->length));
	ir->index.allocated_size = ir->index.index_length;
	
	if (ntfs_resident_attr_value_resize(ctx->mrec, ctx->attr,
			sizeof(INDEX_ROOT) - sizeof(INDEX_HEADER) +
			le32_to_cpu(ir->index.allocated_size)))
		/* FIXME: revert bitmap, index root */
		goto err_out;
	/*
	 *  FIXME: do it earlier if we have enough space in IR (should always),
	 *  so in error case we wouldn't lose the IB.
	 */
	ntfs_ie_set_vcn(ie, new_ib_vcn);
	
	ret = STATUS_OK;
err_out:
	free(ib);
	ntfs_attr_put_search_ctx(ctx);
	return ret;
clear_bmp:
	ntfs_ibm_clear(icx, new_ib_vcn);
	goto err_out;
}

/**
 * ntfs_ir_truncate - Truncate index root attribute
 * 
 * Returns STATUS_OK, STATUS_RESIDENT_ATTRIBUTE_FILLED_MFT or STATUS_ERROR.
 */
static int ntfs_ir_truncate(ntfs_index_context *icx, int data_size)
{			  
	ntfs_attr *na;
	int ret;

	ntfs_log_trace("Entering\n");
	
	na = ntfs_attr_open(icx->ni, AT_INDEX_ROOT, icx->name, icx->name_len);
	if (!na) {
		ntfs_log_perror("Failed to open INDEX_ROOT");
		return STATUS_ERROR;
	}
	/*
	 *  INDEX_ROOT must be resident and its entries can be moved to 
	 *  INDEX_BLOCK, so ENOSPC isn't a real error.
	 */
	ret = ntfs_attr_truncate(na, data_size + offsetof(INDEX_ROOT, index));
	if (ret == STATUS_OK) {
		
		ntfs_attr_search_ctx *ctx;
		
		icx->ir = ntfs_ir_lookup(icx->ni, icx->name, icx->name_len, &ctx);
		if (!icx->ir)
			return STATUS_ERROR;
	
		icx->ir->index.allocated_size = cpu_to_le32(data_size);
		
		ntfs_attr_put_search_ctx(ctx);
	
	} else if (errno != ENOSPC)
		ntfs_log_trace("Failed to truncate INDEX_ROOT");
	
	ntfs_attr_close(na);
	return ret;
}
		
/**
 * ntfs_ir_make_space - Make more space for the index root attribute
 * 
 * On success return STATUS_OK or STATUS_KEEP_SEARCHING.
 * On error return STATUS_ERROR.
 */
static int ntfs_ir_make_space(ntfs_index_context *icx, int data_size)
{			  
	int ret;

	ntfs_log_trace("Entering\n");
	
	ret = ntfs_ir_truncate(icx, data_size);
	if (ret == STATUS_RESIDENT_ATTRIBUTE_FILLED_MFT) {
		
		ret = ntfs_ir_reparent(icx);
		if (ret == STATUS_OK)
			ret = STATUS_KEEP_SEARCHING;
		else
			ntfs_log_perror("Failed to nodify INDEX_ROOT");
	} 

	return ret;
}

/*
 * NOTE: 'ie' must be a copy of a real index entry.
 */
static int ntfs_ie_add_vcn(INDEX_ENTRY **ie)
{
	INDEX_ENTRY *p, *old = *ie;
	 
	old->length = cpu_to_le16(le16_to_cpu(old->length) + sizeof(VCN));
	p = realloc(old, le16_to_cpu(old->length));
	if (!p)
		return STATUS_ERROR;
	
	p->ie_flags |= INDEX_ENTRY_NODE;
	*ie = p;

	return STATUS_OK;
}

static int ntfs_ih_insert(INDEX_HEADER *ih, INDEX_ENTRY *orig_ie, VCN new_vcn, 
			  int pos)
{
	INDEX_ENTRY *ie_node, *ie;
	int ret = STATUS_ERROR;
	VCN old_vcn;
	
	ntfs_log_trace("Entering\n");
	
	ie = ntfs_ie_dup(orig_ie);
	if (!ie)
		return STATUS_ERROR;
	
	if (!(ie->ie_flags & INDEX_ENTRY_NODE))
		if (ntfs_ie_add_vcn(&ie))
			goto out;

	ie_node = ntfs_ie_get_by_pos(ih, pos);
	old_vcn = ntfs_ie_get_vcn(ie_node);
	ntfs_ie_set_vcn(ie_node, new_vcn);
	
	ntfs_ie_insert(ih, ie, ie_node);
	ntfs_ie_set_vcn(ie_node, old_vcn);
	ret = STATUS_OK;
out:	
	free(ie);
	
	return ret;
}

static VCN ntfs_icx_parent_vcn(ntfs_index_context *icx)
{
	return icx->parent_vcn[icx->pindex];
}

static VCN ntfs_icx_parent_pos(ntfs_index_context *icx)
{
	return icx->parent_pos[icx->pindex];
}


static int ntfs_ir_insert_median(ntfs_index_context *icx, INDEX_ENTRY *median,
				 VCN new_vcn)
{
	u32 new_size;
	int ret;
	
	ntfs_log_trace("Entering\n");
	
	new_size = le32_to_cpu(icx->ir->index.index_length) + 
			le16_to_cpu(median->length);
	if (!(median->ie_flags & INDEX_ENTRY_NODE))
		new_size += sizeof(VCN);

	ret = ntfs_ir_make_space(icx, new_size);
	if (ret != STATUS_OK)
		return ret;

	return ntfs_ih_insert(&icx->ir->index, median, new_vcn, (int)ntfs_icx_parent_pos(icx));
}

static int ntfs_ib_split(ntfs_index_context *icx, INDEX_BLOCK *ib);

/**
 * On success return STATUS_OK or STATUS_KEEP_SEARCHING.
 * On error return STATUS_ERROR.
 */
static int ntfs_ib_insert(ntfs_index_context *icx, INDEX_ENTRY *ie, VCN new_vcn)
{			  
	INDEX_BLOCK *ib;
	u32 idx_size, allocated_size;
	int err = STATUS_ERROR;
	VCN old_vcn;

	ntfs_log_trace("Entering\n");
	
	ib = (INDEX_BLOCK *) ntfs_malloc(icx->block_size);
	if (!ib)
		return -1;
	
	old_vcn = ntfs_icx_parent_vcn(icx);
	
	if (ntfs_ib_read(icx, old_vcn, ib))
		goto err_out;

	idx_size       = le32_to_cpu(ib->index.index_length);
	allocated_size = le32_to_cpu(ib->index.allocated_size);
	/* FIXME: sizeof(VCN) should be included only if ie has no VCN */
	if (idx_size + le16_to_cpu(ie->length) + sizeof(VCN) > allocated_size) {
		err = ntfs_ib_split(icx, ib);
		if (err == STATUS_OK)
			err = STATUS_KEEP_SEARCHING;
		goto err_out;
	}
	
	if (ntfs_ih_insert(&ib->index, ie, new_vcn, ntfs_icx_parent_pos(icx)))
		goto err_out;
	
	if (ntfs_ib_write(icx, old_vcn, ib))
		goto err_out;
	
	err = STATUS_OK;
err_out:	
	free(ib);
	return err;
}

/**
 * ntfs_ib_split - Split index allocation attribute
 * 
 * On success return STATUS_OK or STATUS_KEEP_SEARCHING.
 * On error return is STATUS_ERROR.
 */
static int ntfs_ib_split(ntfs_index_context *icx, INDEX_BLOCK *ib)
{			  
	INDEX_ENTRY *median;
	VCN new_vcn;
	int ret;

	ntfs_log_trace("Entering\n");
	
	if (ntfs_icx_parent_dec(icx))
		return STATUS_ERROR;
	
	median  = ntfs_ie_get_median(&ib->index);
	new_vcn = ntfs_ibm_get_free(icx);
	if (new_vcn == -1)
		return STATUS_ERROR;
	
	if (ntfs_ib_copy_tail(icx, ib, median, new_vcn)) {
		ntfs_ibm_clear(icx, new_vcn);
		return STATUS_ERROR;
	}
	
	if (ntfs_icx_parent_vcn(icx) == VCN_INDEX_ROOT_PARENT)
		ret = ntfs_ir_insert_median(icx, median, new_vcn);
	else
		ret = ntfs_ib_insert(icx, median, new_vcn);
	
	ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
	
	if (ret != STATUS_OK) {
		ntfs_ibm_clear(icx, new_vcn);
		return ret;
	}
	
	ret = ntfs_ib_cut_tail(icx, ib, median);
	
	return ret;
}


static int ntfs_ie_add(ntfs_index_context *icx, INDEX_ENTRY *ie)
{
	char *fn;
	INDEX_HEADER *ih;
	int allocated_size, new_size;
	int ret = STATUS_ERROR;
	
	fn = ntfs_ie_filename_get(ie);
	ntfs_log_trace("file: '%s'\n", fn);
	
	while (1) {
				
		if (!ntfs_index_lookup(&ie->key, le16_to_cpu(ie->key_length), icx)) {
			errno = EEXIST;
			ntfs_log_perror("Index already have such entry");
			goto err_out;
		}
		if (errno != ENOENT) {
			ntfs_log_perror("Failed to find place for new entry");
			goto err_out;
		}
		
		if (icx->is_in_root)
			ih = &icx->ir->index;
		else
			ih = &icx->ib->index;
		
		allocated_size = le32_to_cpu(ih->allocated_size);
		new_size = le32_to_cpu(ih->index_length) + le16_to_cpu(ie->length);
	
		if (new_size <= allocated_size)
			break;
		
		ntfs_log_trace("index block sizes: allocated: %d  needed: %d\n",
			       allocated_size, new_size);
		
		if (icx->is_in_root) {
			if (ntfs_ir_make_space(icx, new_size) == STATUS_ERROR)
				goto err_out;
		} else {
			if (ntfs_ib_split(icx, icx->ib) == STATUS_ERROR)
				goto err_out;
		}
		
		ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
		ntfs_index_ctx_reinit(icx);
	}
	
	ntfs_ie_insert(ih, ie, icx->entry);
	ntfs_index_entry_mark_dirty(icx);
	
	ret = STATUS_OK;
err_out:
	ntfs_attr_name_free(&fn);
	ntfs_log_trace("%s\n", ret ? "Failed" : "Done");
	return ret;
}

/**
 * ntfs_index_add_filename - add filename to directory index
 * @ni:		ntfs inode describing directory to which index add filename
 * @fn:		FILE_NAME attribute to add
 * @mref:	reference of the inode which @fn describes
 *
 * Return 0 on success or -1 on error with errno set to the error code.
 */
int ntfs_index_add_filename(ntfs_inode *ni, FILE_NAME_ATTR *fn, MFT_REF mref)
{
	INDEX_ENTRY *ie;
	ntfs_index_context *icx;
	int fn_size, ie_size, ret = -1;

	ntfs_log_trace("Entering\n");
	
	if (!ni || !fn) {
		ntfs_log_error("Invalid arguments.\n");
		errno = EINVAL;
		return -1;
	}
	
	fn_size = (fn->file_name_length * sizeof(ntfschar)) +
			sizeof(FILE_NAME_ATTR);
	ie_size = (sizeof(INDEX_ENTRY_HEADER) + fn_size + 7) & ~7;
	
	ie = ntfs_calloc(ie_size);
	if (!ie)
		return -1;

	ie->indexed_file = cpu_to_le64(mref);
	ie->length 	 = cpu_to_le16(ie_size);
	ie->key_length 	 = cpu_to_le16(fn_size);
	memcpy(&ie->key, fn, fn_size);
	
	icx = ntfs_index_ctx_get(ni, NTFS_INDEX_I30, 4);
	if (!icx)
		goto out;
	
	ret = ntfs_ie_add(icx, ie);
	
	ntfs_index_ctx_put(icx);
out:
	free(ie);
	return ret;
}

static int ntfs_ih_takeout(ntfs_index_context *icx, INDEX_HEADER *ih,
			   INDEX_ENTRY *ie, INDEX_BLOCK *ib)
{
	INDEX_ENTRY *ie_roam;
	int ret = STATUS_ERROR;
	
	ntfs_log_trace("Entering\n");
	
	ie_roam = ntfs_ie_dup_novcn(ie);
	if (!ie_roam)
		return STATUS_ERROR;

	ntfs_ie_delete(ih, ie);

	if (ntfs_icx_parent_vcn(icx) == VCN_INDEX_ROOT_PARENT)
		ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
	else
		if (ntfs_ib_write(icx, ntfs_icx_parent_vcn(icx), ib))
			goto out;
	
	ntfs_index_ctx_reinit(icx);

	ret = ntfs_ie_add(icx, ie_roam);
out:
	free(ie_roam);
	return ret;
}

/**
 *  Used if an empty index block to be deleted has END entry as the parent
 *  in the INDEX_ROOT which is the only one there.
 */
static void ntfs_ir_leafify(ntfs_index_context *icx, INDEX_HEADER *ih)
{
	INDEX_ENTRY *ie;
	
	ntfs_log_trace("Entering\n");
	
	ie = ntfs_ie_get_first(ih);
	ie->ie_flags &= ~INDEX_ENTRY_NODE;
	ie->length = cpu_to_le16(le16_to_cpu(ie->length) - sizeof(VCN));
	
	ih->index_length = cpu_to_le32(le32_to_cpu(ih->index_length) - sizeof(VCN));
	ih->ih_flags &= ~LARGE_INDEX;
	
	/* Not fatal error */
	ntfs_ir_truncate(icx, le32_to_cpu(ih->index_length));
	
	ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
	ntfs_index_ctx_reinit(icx);
}

/**
 *  Used if an empty index block to be deleted has END entry as the parent 
 *  in the INDEX_ROOT which is not the only one there.
 */
static int ntfs_ih_reparent_end(ntfs_index_context *icx, INDEX_HEADER *ih,
				INDEX_BLOCK *ib)
{
	INDEX_ENTRY *ie, *ie_prev;
	
	ntfs_log_trace("Entering\n");
	
	ie = ntfs_ie_get_by_pos(ih, ntfs_icx_parent_pos(icx));
	ie_prev = ntfs_ie_prev(ih, ie);
	
	ntfs_ie_set_vcn(ie, ntfs_ie_get_vcn(ie_prev));
	
	return ntfs_ih_takeout(icx, ih, ie_prev, ib);
}

static int ntfs_index_rm_leaf(ntfs_index_context *icx)
{
	INDEX_BLOCK *ib = NULL;
	INDEX_HEADER *parent_ih;
	INDEX_ENTRY *ie;
	int ret = STATUS_ERROR;
	
	ntfs_log_trace("pindex: %d\n", icx->pindex);
	
	if (ntfs_icx_parent_dec(icx))
		return STATUS_ERROR;

	if (ntfs_ibm_clear(icx, icx->parent_vcn[icx->pindex + 1]))
		return STATUS_ERROR;
	
	if (ntfs_icx_parent_vcn(icx) == VCN_INDEX_ROOT_PARENT)
		parent_ih = &icx->ir->index;
	else {
		ib = (INDEX_BLOCK *) ntfs_malloc(icx->block_size);
		if (!ib)
			return STATUS_ERROR;
		
		if (ntfs_ib_read(icx, ntfs_icx_parent_vcn(icx), ib))
			goto out;
	
		parent_ih = &ib->index;
	}
	
	ie = ntfs_ie_get_by_pos(parent_ih, ntfs_icx_parent_pos(icx));
	if (!ntfs_ie_end(ie)) {
		ret = ntfs_ih_takeout(icx, parent_ih, ie, ib);
		goto out;
	}
		
	if (ntfs_ih_zero_entry(parent_ih)) {
		
		if (ntfs_icx_parent_vcn(icx) == VCN_INDEX_ROOT_PARENT) {
			ntfs_ir_leafify(icx, parent_ih);
			goto ok;
		}
		
		ret = ntfs_index_rm_leaf(icx);
		goto out;
	}
		
	if (ntfs_ih_reparent_end(icx, parent_ih, ib))
		goto out;
ok:	
	ret = STATUS_OK;
out:
	free(ib);
	return ret;
}

static int ntfs_index_rm_node(ntfs_index_context *icx)
{
	int entry_pos;
	VCN vcn;
	INDEX_BLOCK *ib = NULL;
	INDEX_ENTRY *ie_succ, *ie, *entry = icx->entry;
	INDEX_HEADER *ih;
	u32 new_size;
	int delta, ret = STATUS_ERROR;

	ntfs_log_trace("Entering\n");
	
	if (!icx->ia_na) {
		icx->ia_na = ntfs_ia_open(icx, icx->ni);
		if (!icx->ia_na)
			return STATUS_ERROR;
	}

	ib = (INDEX_BLOCK *) ntfs_malloc(icx->block_size);
	if (!ib)
		return STATUS_ERROR;
	
	ie_succ = ntfs_ie_get_next(icx->entry);
	entry_pos = icx->parent_pos[icx->pindex]++;
descend:
	vcn = ntfs_ie_get_vcn(ie_succ);
	if (ntfs_ib_read(icx, vcn, ib))
		goto out;
	
	ie_succ = ntfs_ie_get_first(&ib->index);

	if (ntfs_icx_parent_inc(icx))
		goto out;
	
	icx->parent_vcn[icx->pindex] = vcn;
	icx->parent_pos[icx->pindex] = 0;

	if ((ib->index.ih_flags & NODE_MASK) == INDEX_NODE)
		goto descend;

	if (ntfs_ih_zero_entry(&ib->index)) {
		errno = 1045;
		ntfs_log_perror("Failed to find any entry in an index block. "
				"Please run chkdsk.");
		goto out;
	}

	ie = ntfs_ie_dup(ie_succ);
	if (!ie)
		goto out;
	
	if (ntfs_ie_add_vcn(&ie))
		goto out2;

	ntfs_ie_set_vcn(ie, ntfs_ie_get_vcn(icx->entry));

	if (icx->is_in_root)
		ih = &icx->ir->index;
	else
		ih = &icx->ib->index;

	delta = le16_to_cpu(ie->length) - le16_to_cpu(icx->entry->length);
	new_size = le32_to_cpu(ih->index_length) + delta;
	if (delta > 0) {
		if (icx->is_in_root) {
			if (ntfs_ir_truncate(icx, new_size)) {
				errno = 1045;
				ntfs_log_perror("Denied to truncate INDEX ROOT during entry removal");
				goto out2;
			}
			
			ih = &icx->ir->index;
			entry = ntfs_ie_get_by_pos(ih, entry_pos);
			
		} else if (new_size > le32_to_cpu(ih->allocated_size)) {
			errno = /*EOPNOTSUPP*/1045 ;
			ntfs_log_perror("Denied to split INDEX BLOCK during entry removal");
			goto out2;
		}
	}

	ntfs_ie_delete(ih, entry);
	ntfs_ie_insert(ih, ie, entry);
	
	if (icx->is_in_root) {
		if (ntfs_ir_truncate(icx, new_size))
			goto out2;
		ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
	} else
		if (ntfs_icx_ib_write(icx))
			goto out2;
	
	ntfs_ie_delete(&ib->index, ie_succ);
	
	if (ntfs_ih_zero_entry(&ib->index)) {
		if (ntfs_index_rm_leaf(icx))
			goto out2;
	} else 
		if (ntfs_ib_write(icx, vcn, ib))
			goto out2;

	ret = STATUS_OK;
out2:
	free(ie);
out:
	free(ib);
	return ret;
}



int 
ntfs_index_rm(
	IN ntfs_index_context *icx
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/05 [5:3:2009 - 23:30]

Routine Description:
  remove entry from the index.
  Delete entry described by @icx from the index. Index context is always 
  reinitialized after use of this function, so it can be used for index lookup once again.
    
Arguments:
  icx:	index context describing entry to delete

Return Value:
  Return 0 on success or -1 on error with errno set to the error code.
    
--*/
{
	INDEX_HEADER *ih;
	int err;

	ntfs_log_trace("Entering\n");
	
	if (!icx || (!icx->ib && !icx->ir) || ntfs_ie_end(icx->entry)) {
		ntfs_log_error("Invalid arguments.\n");
		errno = EINVAL;
		goto err_out;
	}

	if (icx->is_in_root)
		ih = &icx->ir->index;
	else
		ih = &icx->ib->index;
	
	if (icx->entry->ie_flags & INDEX_ENTRY_NODE) {
		
		if (ntfs_index_rm_node(icx))
			goto err_out;

	} else if (icx->is_in_root || !ntfs_ih_one_entry(ih)) {
		
		ntfs_ie_delete(ih, icx->entry);
		
		if (icx->is_in_root) {
			err = ntfs_ir_truncate(icx, le32_to_cpu(ih->index_length));
			if (err != STATUS_OK)
				goto err_out;
		} else
			if (ntfs_icx_ib_write(icx))
				goto err_out;
	} else {
		if (ntfs_index_rm_leaf(icx))
			goto err_out;
	}
		
	ntfs_index_ctx_reinit(icx);
	ntfs_log_trace("Done.\n");
	return 0;
err_out:
	err = errno;
	ntfs_index_ctx_reinit(icx);
	errno = err;
	ntfs_log_trace("Failed.\n");
	return -1;
}

/**
 * ntfs_index_root_get - read the index root of an attribute
 * @ni:		open ntfs inode in which the ntfs attribute resides
 * @attr:	attribute for which we want its index root 
 *
 * This function will read the related index root an ntfs attribute.
 *
 * On success a buffer is allocated with the content of the index root
 * and which needs to be freed when it's not needed anymore.
 *
 * On error NULL is returned with errno set to the error code.
 */
INDEX_ROOT *ntfs_index_root_get(ntfs_inode *ni, ATTR_RECORD *attr)
{
	ntfs_attr_search_ctx *ctx;
	ntfschar *name;
	INDEX_ROOT *root = NULL;

	name = (ntfschar *)((u8 *)attr + le16_to_cpu(attr->name_offset));

	if (!ntfs_ir_lookup(ni, name, attr->name_length, &ctx))
		return NULL;
	
	root = (INDEX_ROOT *) ntfs_malloc(sizeof(INDEX_ROOT));
	if (!root)
		goto out;
	
	*root = *((INDEX_ROOT *)((u8 *)ctx->attr +
				le16_to_cpu(ctx->attr->X.value_offset)));
out:	
	ntfs_attr_put_search_ctx(ctx);
	return root;
}

