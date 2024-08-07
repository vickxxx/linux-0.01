/*
 * linux/fs/ext3/xattr.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 *
 * Fix by Harrison Xing <harrison@mountainviewdata.com>.
 * Ext3 code with a lot of help from Eric Jarman <ejarman@acm.org>.
 * Extended attributes for symlinks and special files added per
 *  suggestion of Luka Renko <luka.renko@hermes.si>.
 */

/*
 * Extended attributes are stored on disk blocks allocated outside of
 * any inode. The i_file_acl field is then made to point to this allocated
 * block. If all extended attributes of an inode are identical, these
 * inodes may share the same extended attribute block. Such situations
 * are automatically detected by keeping a cache of recent attribute block
 * numbers and hashes over the block's contents in memory.
 *
 *
 * Extended attribute block layout:
 *
 *   +------------------+
 *   | header           |
 *   � entry 1          | |
 *   | entry 2          | | growing downwards
 *   | entry 3          | v
 *   | four null bytes  |
 *   | . . .            |
 *   | value 1          | ^
 *   | value 3          | | growing upwards
 *   | value 2          | |
 *   +------------------+
 *
 * The block header is followed by multiple entry descriptors. These entry
 * descriptors are variable in size, and alligned to EXT3_XATTR_PAD
 * byte boundaries. The entry descriptors are sorted by attribute name,
 * so that two extended attribute blocks can be compared efficiently.
 *
 * Attribute values are aligned to the end of the block, stored in
 * no specific order. They are also padded to EXT3_XATTR_PAD byte
 * boundaries. No additional gaps are left between them.
 *
 * Locking strategy
 * ----------------
 * EXT3_I(inode)->i_file_acl is protected by EXT3_I(inode)->xattr_sem.
 * EA blocks are only changed if they are exclusive to an inode, so
 * holding xattr_sem also means that nothing but the EA block's reference
 * count will change. Multiple writers to an EA block are synchronized
 * by the bh lock. No more than a single bh lock is held at any time
 * to avoid deadlocks.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/ext3_jbd.h>
#include <linux/ext3_fs.h>
#include <linux/mbcache.h>
#include <linux/quotaops.h>
#include <linux/rwsem.h>
#include "xattr.h"
#include "acl.h"

#define HDR(bh) ((struct ext3_xattr_header *)((bh)->b_data))
#define ENTRY(ptr) ((struct ext3_xattr_entry *)(ptr))
#define FIRST_ENTRY(bh) ENTRY(HDR(bh)+1)
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

#ifdef EXT3_XATTR_DEBUG
# define ea_idebug(inode, f...) do { \
		printk(KERN_DEBUG "inode %s:%ld: ", \
			inode->i_sb->s_id, inode->i_ino); \
		printk(f); \
		printk("\n"); \
	} while (0)
# define ea_bdebug(bh, f...) do { \
		char b[BDEVNAME_SIZE]; \
		printk(KERN_DEBUG "block %s:%lu: ", \
			bdevname(bh->b_bdev, b), \
			(unsigned long) bh->b_blocknr); \
		printk(f); \
		printk("\n"); \
	} while (0)
#else
# define ea_idebug(f...)
# define ea_bdebug(f...)
#endif

static int ext3_xattr_set_handle2(handle_t *, struct inode *,
				  struct buffer_head *,
				  struct ext3_xattr_header *);

static int ext3_xattr_cache_insert(struct buffer_head *);
static struct buffer_head *ext3_xattr_cache_find(handle_t *, struct inode *,
						 struct ext3_xattr_header *,
						 int *);
static void ext3_xattr_cache_remove(struct buffer_head *);
static void ext3_xattr_rehash(struct ext3_xattr_header *,
			      struct ext3_xattr_entry *);

static struct mb_cache *ext3_xattr_cache;
static struct ext3_xattr_handler *ext3_xattr_handlers[EXT3_XATTR_INDEX_MAX];
static rwlock_t ext3_handler_lock = RW_LOCK_UNLOCKED;

int
ext3_xattr_register(int name_index, struct ext3_xattr_handler *handler)
{
	int error = -EINVAL;

	if (name_index > 0 && name_index <= EXT3_XATTR_INDEX_MAX) {
		write_lock(&ext3_handler_lock);
		if (!ext3_xattr_handlers[name_index-1]) {
			ext3_xattr_handlers[name_index-1] = handler;
			error = 0;
		}
		write_unlock(&ext3_handler_lock);
	}
	return error;
}

void
ext3_xattr_unregister(int name_index, struct ext3_xattr_handler *handler)
{
	if (name_index > 0 || name_index <= EXT3_XATTR_INDEX_MAX) {
		write_lock(&ext3_handler_lock);
		ext3_xattr_handlers[name_index-1] = NULL;
		write_unlock(&ext3_handler_lock);
	}
}

static inline const char *
strcmp_prefix(const char *a, const char *a_prefix)
{
	while (*a_prefix && *a == *a_prefix) {
		a++;
		a_prefix++;
	}
	return *a_prefix ? NULL : a;
}

/*
 * Decode the extended attribute name, and translate it into
 * the name_index and name suffix.
 */
static inline struct ext3_xattr_handler *
ext3_xattr_resolve_name(const char **name)
{
	struct ext3_xattr_handler *handler = NULL;
	int i;

	if (!*name)
		return NULL;
	read_lock(&ext3_handler_lock);
	for (i=0; i<EXT3_XATTR_INDEX_MAX; i++) {
		if (ext3_xattr_handlers[i]) {
			const char *n = strcmp_prefix(*name,
				ext3_xattr_handlers[i]->prefix);
			if (n) {
				handler = ext3_xattr_handlers[i];
				*name = n;
				break;
			}
		}
	}
	read_unlock(&ext3_handler_lock);
	return handler;
}

static inline struct ext3_xattr_handler *
ext3_xattr_handler(int name_index)
{
	struct ext3_xattr_handler *handler = NULL;
	if (name_index > 0 && name_index <= EXT3_XATTR_INDEX_MAX) {
		read_lock(&ext3_handler_lock);
		handler = ext3_xattr_handlers[name_index-1];
		read_unlock(&ext3_handler_lock);
	}
	return handler;
}

/*
 * Inode operation getxattr()
 *
 * dentry->d_inode->i_sem: don't care
 */
ssize_t
ext3_getxattr(struct dentry *dentry, const char *name,
	      void *buffer, size_t size)
{
	struct ext3_xattr_handler *handler;
	struct inode *inode = dentry->d_inode;

	handler = ext3_xattr_resolve_name(&name);
	if (!handler)
		return -EOPNOTSUPP;
	return handler->get(inode, name, buffer, size);
}

/*
 * Inode operation listxattr()
 *
 * dentry->d_inode->i_sem: don't care
 */
ssize_t
ext3_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	return ext3_xattr_list(dentry->d_inode, buffer, size);
}

/*
 * Inode operation setxattr()
 *
 * dentry->d_inode->i_sem: down
 */
int
ext3_setxattr(struct dentry *dentry, const char *name,
	      const void *value, size_t size, int flags)
{
	struct ext3_xattr_handler *handler;
	struct inode *inode = dentry->d_inode;

	if (size == 0)
		value = "";  /* empty EA, do not remove */
	handler = ext3_xattr_resolve_name(&name);
	if (!handler)
		return -EOPNOTSUPP;
	return handler->set(inode, name, value, size, flags);
}

/*
 * Inode operation removexattr()
 *
 * dentry->d_inode->i_sem: down
 */
int
ext3_removexattr(struct dentry *dentry, const char *name)
{
	struct ext3_xattr_handler *handler;
	struct inode *inode = dentry->d_inode;

	handler = ext3_xattr_resolve_name(&name);
	if (!handler)
		return -EOPNOTSUPP;
	return handler->set(inode, name, NULL, 0, XATTR_REPLACE);
}

/*
 * ext3_xattr_get()
 *
 * Copy an extended attribute into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
int
ext3_xattr_get(struct inode *inode, int name_index, const char *name,
	       void *buffer, size_t buffer_size)
{
	struct buffer_head *bh = NULL;
	struct ext3_xattr_entry *entry;
	size_t name_len, size;
	char *end;
	int error;

	ea_idebug(inode, "name=%d.%s, buffer=%p, buffer_size=%ld",
		  name_index, name, buffer, (long)buffer_size);

	if (name == NULL)
		return -EINVAL;
	down_read(&EXT3_I(inode)->xattr_sem);
	error = -ENODATA;
	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %d", EXT3_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	error = -EIO;
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(HDR(bh)->h_refcount));
	end = bh->b_data + bh->b_size;
	if (HDR(bh)->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
	    HDR(bh)->h_blocks != cpu_to_le32(1)) {
bad_block:	ext3_error(inode->i_sb, "ext3_xattr_get",
			"inode %ld: bad block %d", inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}
	/* find named attribute */
	name_len = strlen(name);

	error = -ERANGE;
	if (name_len > 255)
		goto cleanup;
	entry = FIRST_ENTRY(bh);
	while (!IS_LAST_ENTRY(entry)) {
		struct ext3_xattr_entry *next =
			EXT3_XATTR_NEXT(entry);
		if ((char *)next >= end)
			goto bad_block;
		if (name_index == entry->e_name_index &&
		    name_len == entry->e_name_len &&
		    memcmp(name, entry->e_name, name_len) == 0)
			goto found;
		entry = next;
	}
	/* Check the remaining name entries */
	while (!IS_LAST_ENTRY(entry)) {
		struct ext3_xattr_entry *next =
			EXT3_XATTR_NEXT(entry);
		if ((char *)next >= end)
			goto bad_block;
		entry = next;
	}
	if (ext3_xattr_cache_insert(bh))
		ea_idebug(inode, "cache insert failed");
	error = -ENODATA;
	goto cleanup;
found:
	/* check the buffer size */
	if (entry->e_value_block != 0)
		goto bad_block;
	size = le32_to_cpu(entry->e_value_size);
	if (size > inode->i_sb->s_blocksize ||
	    le16_to_cpu(entry->e_value_offs) + size > inode->i_sb->s_blocksize)
		goto bad_block;

	if (ext3_xattr_cache_insert(bh))
		ea_idebug(inode, "cache insert failed");
	if (buffer) {
		error = -ERANGE;
		if (size > buffer_size)
			goto cleanup;
		/* return value of attribute */
		memcpy(buffer, bh->b_data + le16_to_cpu(entry->e_value_offs),
			size);
	}
	error = size;

cleanup:
	brelse(bh);
	up_read(&EXT3_I(inode)->xattr_sem);

	return error;
}

/*
 * ext3_xattr_list()
 *
 * Copy a list of attribute names into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
int
ext3_xattr_list(struct inode *inode, char *buffer, size_t buffer_size)
{
	struct buffer_head *bh = NULL;
	struct ext3_xattr_entry *entry;
	size_t size = 0;
	char *buf, *end;
	int error;

	ea_idebug(inode, "buffer=%p, buffer_size=%ld",
		  buffer, (long)buffer_size);

	down_read(&EXT3_I(inode)->xattr_sem);
	error = 0;
	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %d", EXT3_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	error = -EIO;
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(HDR(bh)->h_refcount));
	end = bh->b_data + bh->b_size;
	if (HDR(bh)->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
	    HDR(bh)->h_blocks != cpu_to_le32(1)) {
bad_block:	ext3_error(inode->i_sb, "ext3_xattr_list",
			"inode %ld: bad block %d", inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}
	/* compute the size required for the list of attribute names */
	for (entry = FIRST_ENTRY(bh); !IS_LAST_ENTRY(entry);
	     entry = EXT3_XATTR_NEXT(entry)) {
		struct ext3_xattr_handler *handler;
		struct ext3_xattr_entry *next =
			EXT3_XATTR_NEXT(entry);
		if ((char *)next >= end)
			goto bad_block;

		handler = ext3_xattr_handler(entry->e_name_index);
		if (handler)
			size += handler->list(NULL, inode, entry->e_name,
					      entry->e_name_len);
	}

	if (ext3_xattr_cache_insert(bh))
		ea_idebug(inode, "cache insert failed");
	if (!buffer) {
		error = size;
		goto cleanup;
	} else {
		error = -ERANGE;
		if (size > buffer_size)
			goto cleanup;
	}

	/* list the attribute names */
	buf = buffer;
	for (entry = FIRST_ENTRY(bh); !IS_LAST_ENTRY(entry);
	     entry = EXT3_XATTR_NEXT(entry)) {
		struct ext3_xattr_handler *handler;

		handler = ext3_xattr_handler(entry->e_name_index);
		if (handler)
			buf += handler->list(buf, inode, entry->e_name,
					     entry->e_name_len);
	}
	error = size;

cleanup:
	brelse(bh);
	up_read(&EXT3_I(inode)->xattr_sem);

	return error;
}

/*
 * If the EXT3_FEATURE_COMPAT_EXT_ATTR feature of this file system is
 * not set, set it.
 */
static void ext3_xattr_update_super_block(handle_t *handle,
					  struct super_block *sb)
{
	if (EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_EXT_ATTR))
		return;

	lock_super(sb);
	if (ext3_journal_get_write_access(handle, EXT3_SB(sb)->s_sbh) == 0) {
		EXT3_SB(sb)->s_es->s_feature_compat |=
			cpu_to_le32(EXT3_FEATURE_COMPAT_EXT_ATTR);
		sb->s_dirt = 1;
		ext3_journal_dirty_metadata(handle, EXT3_SB(sb)->s_sbh);
	}
	unlock_super(sb);
}

/*
 * ext3_xattr_set_handle()
 *
 * Create, replace or remove an extended attribute for this inode. Buffer
 * is NULL to remove an existing extended attribute, and non-NULL to
 * either replace an existing extended attribute, or create a new extended
 * attribute. The flags XATTR_REPLACE and XATTR_CREATE
 * specify that an extended attribute must exist and must not exist
 * previous to the call, respectively.
 *
 * Returns 0, or a negative error number on failure.
 */
int
ext3_xattr_set_handle(handle_t *handle, struct inode *inode, int name_index,
		      const char *name, const void *value, size_t value_len,
		      int flags)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = NULL;
	struct ext3_xattr_header *header = NULL;
	struct ext3_xattr_entry *here, *last;
	size_t name_len, free, min_offs = sb->s_blocksize;
	int not_found = 1, error;
	char *end;

	/*
	 * header -- Points either into bh, or to a temporarily
	 *           allocated buffer.
	 * here -- The named entry found, or the place for inserting, within
	 *         the block pointed to by header.
	 * last -- Points right after the last named entry within the block
	 *         pointed to by header.
	 * min_offs -- The offset of the first value (values are aligned
	 *             towards the end of the block).
	 * end -- Points right after the block pointed to by header.
	 */

	ea_idebug(inode, "name=%d.%s, value=%p, value_len=%ld",
		  name_index, name, value, (long)value_len);

	if (IS_RDONLY(inode))
		return -EROFS;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		return -EPERM;
	if (value == NULL)
		value_len = 0;
	if (name == NULL)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > 255 || value_len > sb->s_blocksize)
		return -ERANGE;
	down_write(&EXT3_I(inode)->xattr_sem);
	if (EXT3_I(inode)->i_file_acl) {
		/* The inode already has an extended attribute block. */
		bh = sb_bread(sb, EXT3_I(inode)->i_file_acl);
		error = -EIO;
		if (!bh)
			goto cleanup;
		ea_bdebug(bh, "b_count=%d, refcount=%d",
			atomic_read(&(bh->b_count)),
			le32_to_cpu(HDR(bh)->h_refcount));
		header = HDR(bh);
		end = bh->b_data + bh->b_size;
		if (header->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
		    header->h_blocks != cpu_to_le32(1)) {
bad_block:		ext3_error(sb, "ext3_xattr_set",
				"inode %ld: bad block %d", inode->i_ino,
				EXT3_I(inode)->i_file_acl);
			error = -EIO;
			goto cleanup;
		}
		/* Find the named attribute. */
		here = FIRST_ENTRY(bh);
		while (!IS_LAST_ENTRY(here)) {
			struct ext3_xattr_entry *next = EXT3_XATTR_NEXT(here);
			if ((char *)next >= end)
				goto bad_block;
			if (!here->e_value_block && here->e_value_size) {
				size_t offs = le16_to_cpu(here->e_value_offs);
				if (offs < min_offs)
					min_offs = offs;
			}
			not_found = name_index - here->e_name_index;
			if (!not_found)
				not_found = name_len - here->e_name_len;
			if (!not_found)
				not_found = memcmp(name, here->e_name,name_len);
			if (not_found <= 0)
				break;
			here = next;
		}
		last = here;
		/* We still need to compute min_offs and last. */
		while (!IS_LAST_ENTRY(last)) {
			struct ext3_xattr_entry *next = EXT3_XATTR_NEXT(last);
			if ((char *)next >= end)
				goto bad_block;
			if (!last->e_value_block && last->e_value_size) {
				size_t offs = le16_to_cpu(last->e_value_offs);
				if (offs < min_offs)
					min_offs = offs;
			}
			last = next;
		}

		/* Check whether we have enough space left. */
		free = min_offs - ((char*)last - (char*)header) - sizeof(__u32);
	} else {
		/* We will use a new extended attribute block. */
		free = sb->s_blocksize -
			sizeof(struct ext3_xattr_header) - sizeof(__u32);
		here = last = NULL;  /* avoid gcc uninitialized warning. */
	}

	if (not_found) {
		/* Request to remove a nonexistent attribute? */
		error = -ENODATA;
		if (flags & XATTR_REPLACE)
			goto cleanup;
		error = 0;
		if (value == NULL)
			goto cleanup;
	} else {
		/* Request to create an existing attribute? */
		error = -EEXIST;
		if (flags & XATTR_CREATE)
			goto cleanup;
		if (!here->e_value_block && here->e_value_size) {
			size_t size = le32_to_cpu(here->e_value_size);

			if (le16_to_cpu(here->e_value_offs) + size > 
			    sb->s_blocksize || size > sb->s_blocksize)
				goto bad_block;
			free += EXT3_XATTR_SIZE(size);
		}
		free += EXT3_XATTR_LEN(name_len);
	}
	error = -ENOSPC;
	if (free < EXT3_XATTR_LEN(name_len) + EXT3_XATTR_SIZE(value_len))
		goto cleanup;

	/* Here we know that we can set the new attribute. */

	if (header) {
		int credits = 0;

		/* assert(header == HDR(bh)); */
		if (header->h_refcount != cpu_to_le32(1))
			goto skip_get_write_access;
		/* ext3_journal_get_write_access() requires an unlocked bh,
		   which complicates things here. */
		error = ext3_journal_get_write_access_credits(handle, bh,
							      &credits);
		if (error)
			goto cleanup;
		lock_buffer(bh);
		if (header->h_refcount == cpu_to_le32(1)) {
			ea_bdebug(bh, "modifying in-place");
			ext3_xattr_cache_remove(bh);
			/* keep the buffer locked while modifying it. */
		} else {
			int offset;

			unlock_buffer(bh);
			journal_release_buffer(handle, bh, credits);
		skip_get_write_access:
			ea_bdebug(bh, "cloning");
			header = kmalloc(bh->b_size, GFP_KERNEL);
			error = -ENOMEM;
			if (header == NULL)
				goto cleanup;
			memcpy(header, HDR(bh), bh->b_size);
			header->h_refcount = cpu_to_le32(1);
			offset = (char *)header - bh->b_data;
			here = ENTRY((char *)here + offset);
			last = ENTRY((char *)last + offset);
		}
	} else {
		/* Allocate a buffer where we construct the new block. */
		header = kmalloc(sb->s_blocksize, GFP_KERNEL);
		error = -ENOMEM;
		if (header == NULL)
			goto cleanup;
		memset(header, 0, sb->s_blocksize);
		end = (char *)header + sb->s_blocksize;
		header->h_magic = cpu_to_le32(EXT3_XATTR_MAGIC);
		header->h_blocks = header->h_refcount = cpu_to_le32(1);
		last = here = ENTRY(header+1);
	}

	/* Iff we are modifying the block in-place, bh is locked here. */

	if (not_found) {
		/* Insert the new name. */
		size_t size = EXT3_XATTR_LEN(name_len);
		size_t rest = (char *)last - (char *)here;
		memmove((char *)here + size, here, rest);
		memset(here, 0, size);
		here->e_name_index = name_index;
		here->e_name_len = name_len;
		memcpy(here->e_name, name, name_len);
	} else {
		if (!here->e_value_block && here->e_value_size) {
			char *first_val = (char *)header + min_offs;
			size_t offs = le16_to_cpu(here->e_value_offs);
			char *val = (char *)header + offs;
			size_t size = EXT3_XATTR_SIZE(
				le32_to_cpu(here->e_value_size));

			if (size == EXT3_XATTR_SIZE(value_len)) {
				/* The old and the new value have the same
				   size. Just replace. */
				here->e_value_size = cpu_to_le32(value_len);
				memset(val + size - EXT3_XATTR_PAD, 0,
				       EXT3_XATTR_PAD); /* Clear pad bytes. */
				memcpy(val, value, value_len);
				goto skip_replace;
			}

			/* Remove the old value. */
			memmove(first_val + size, first_val, val - first_val);
			memset(first_val, 0, size);
			here->e_value_offs = 0;
			min_offs += size;

			/* Adjust all value offsets. */
			last = ENTRY(header+1);
			while (!IS_LAST_ENTRY(last)) {
				size_t o = le16_to_cpu(last->e_value_offs);
				if (!last->e_value_block && o < offs)
					last->e_value_offs =
						cpu_to_le16(o + size);
				last = EXT3_XATTR_NEXT(last);
			}
		}
		if (value == NULL) {
			/* Remove the old name. */
			size_t size = EXT3_XATTR_LEN(name_len);
			last = ENTRY((char *)last - size);
			memmove(here, (char*)here + size,
				(char*)last - (char*)here);
			memset(last, 0, size);
		}
	}

	if (value != NULL) {
		/* Insert the new value. */
		here->e_value_size = cpu_to_le32(value_len);
		if (value_len) {
			size_t size = EXT3_XATTR_SIZE(value_len);
			char *val = (char *)header + min_offs - size;
			here->e_value_offs =
				cpu_to_le16((char *)val - (char *)header);
			memset(val + size - EXT3_XATTR_PAD, 0,
			       EXT3_XATTR_PAD); /* Clear the pad bytes. */
			memcpy(val, value, value_len);
		}
	}

skip_replace:
	if (IS_LAST_ENTRY(ENTRY(header+1))) {
		/* This block is now empty. */
		if (bh && header == HDR(bh))
			unlock_buffer(bh);  /* we were modifying in-place. */
		error = ext3_xattr_set_handle2(handle, inode, bh, NULL);
	} else {
		ext3_xattr_rehash(header, here);
		if (bh && header == HDR(bh))
			unlock_buffer(bh);  /* we were modifying in-place. */
		error = ext3_xattr_set_handle2(handle, inode, bh, header);
	}

cleanup:
	brelse(bh);
	if (!(bh && header == HDR(bh)))
		kfree(header);
	up_write(&EXT3_I(inode)->xattr_sem);

	return error;
}

/*
 * Second half of ext3_xattr_set_handle(): Update the file system.
 */
static int
ext3_xattr_set_handle2(handle_t *handle, struct inode *inode,
		       struct buffer_head *old_bh,
		       struct ext3_xattr_header *header)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *new_bh = NULL;
	int credits = 0, error;

	if (header) {
		new_bh = ext3_xattr_cache_find(handle, inode, header, &credits);
		if (new_bh) {
			/*
			 * We found an identical block in the cache. The
			 * block returned is locked. The old block will
			 * be released after updating the inode.
			 */
			ea_bdebug(new_bh, "%s block %lu",
				(old_bh == new_bh) ? "keeping" : "reusing",
				(unsigned long) new_bh->b_blocknr);

			error = -EDQUOT;
			if (DQUOT_ALLOC_BLOCK(inode, 1)) {
				unlock_buffer(new_bh);
				journal_release_buffer(handle, new_bh, credits);
				goto cleanup;
			}
			HDR(new_bh)->h_refcount = cpu_to_le32(
				le32_to_cpu(HDR(new_bh)->h_refcount) + 1);
			ea_bdebug(new_bh, "refcount now=%d",
				le32_to_cpu(HDR(new_bh)->h_refcount));
			unlock_buffer(new_bh);
		} else if (old_bh && header == HDR(old_bh)) {
			/* Keep this block. No need to lock the block as we
			 * don't need to change the reference count. */
			new_bh = old_bh;
			get_bh(new_bh);
			ext3_xattr_cache_insert(new_bh);
		} else {
			/* We need to allocate a new block */
			int goal = le32_to_cpu(
					EXT3_SB(sb)->s_es->s_first_data_block) +
				EXT3_I(inode)->i_block_group *
				EXT3_BLOCKS_PER_GROUP(sb);
			int block = ext3_new_block(handle,
				inode, goal, 0, 0, &error);
			if (error)
				goto cleanup;
			ea_idebug(inode, "creating block %d", block);

			new_bh = sb_getblk(sb, block);
			if (!new_bh) {
getblk_failed:
				ext3_free_blocks(handle, inode, block, 1);
				error = -EIO;
				goto cleanup;
			}
			lock_buffer(new_bh);
			error = ext3_journal_get_create_access(handle, new_bh);
			if (error) {
				unlock_buffer(new_bh);
				goto getblk_failed;
			}
			memcpy(new_bh->b_data, header, new_bh->b_size);
			set_buffer_uptodate(new_bh);
			unlock_buffer(new_bh);
			ext3_xattr_cache_insert(new_bh);

			ext3_xattr_update_super_block(handle, sb);
		}
		error = ext3_journal_dirty_metadata(handle, new_bh);
		if (error)
			goto cleanup;
	}

	/* Update the inode. */
	EXT3_I(inode)->i_file_acl = new_bh ? new_bh->b_blocknr : 0;
	inode->i_ctime = CURRENT_TIME;
	ext3_mark_inode_dirty(handle, inode);
	if (IS_SYNC(inode))
		handle->h_sync = 1;

	error = 0;
	if (old_bh && old_bh != new_bh) {
		/*
		 * If there was an old block, and we are no longer using it,
		 * release the old block.
		*/
		error = ext3_journal_get_write_access(handle, old_bh);
		if (error)
			goto cleanup;
		lock_buffer(old_bh);
		if (HDR(old_bh)->h_refcount == cpu_to_le32(1)) {
			/* Free the old block. */
			ea_bdebug(old_bh, "freeing");
			ext3_free_blocks(handle, inode, old_bh->b_blocknr, 1);

			/* ext3_forget() calls bforget() for us, but we
			   let our caller release old_bh, so we need to
			   duplicate the handle before. */
			get_bh(old_bh);
			ext3_forget(handle, 1, inode, old_bh,old_bh->b_blocknr);
		} else {
			/* Decrement the refcount only. */
			HDR(old_bh)->h_refcount = cpu_to_le32(
				le32_to_cpu(HDR(old_bh)->h_refcount) - 1);
			DQUOT_FREE_BLOCK(inode, 1);
			ext3_journal_dirty_metadata(handle, old_bh);
			ea_bdebug(old_bh, "refcount now=%d",
				le32_to_cpu(HDR(old_bh)->h_refcount));
		}
		unlock_buffer(old_bh);
	}

cleanup:
	brelse(new_bh);

	return error;
}

/*
 * ext3_xattr_set()
 *
 * Like ext3_xattr_set_handle, but start from an inode. This extended
 * attribute modification is a filesystem transaction by itself.
 *
 * Returns 0, or a negative error number on failure.
 */
int
ext3_xattr_set(struct inode *inode, int name_index, const char *name,
	       const void *value, size_t value_len, int flags)
{
	handle_t *handle;
	int error, error2;

	handle = ext3_journal_start(inode, EXT3_XATTR_TRANS_BLOCKS);
	if (IS_ERR(handle))
		error = PTR_ERR(handle);
	else
		error = ext3_xattr_set_handle(handle, inode, name_index, name,
					      value, value_len, flags);
	error2 = ext3_journal_stop(handle);

	return error ? error : error2;
}

/*
 * ext3_xattr_delete_inode()
 *
 * Free extended attribute resources associated with this inode. This
 * is called immediately before an inode is freed.
 */
void
ext3_xattr_delete_inode(handle_t *handle, struct inode *inode)
{
	struct buffer_head *bh = NULL;

	down_write(&EXT3_I(inode)->xattr_sem);
	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	if (!bh) {
		ext3_error(inode->i_sb, "ext3_xattr_delete_inode",
			"inode %ld: block %d read error", inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		goto cleanup;
	}
	if (HDR(bh)->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
	    HDR(bh)->h_blocks != cpu_to_le32(1)) {
		ext3_error(inode->i_sb, "ext3_xattr_delete_inode",
			"inode %ld: bad block %d", inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		goto cleanup;
	}
	if (ext3_journal_get_write_access(handle, bh) != 0)
		goto cleanup;
	lock_buffer(bh);
	if (HDR(bh)->h_refcount == cpu_to_le32(1)) {
		ext3_xattr_cache_remove(bh);
		ext3_free_blocks(handle, inode, EXT3_I(inode)->i_file_acl, 1);
		get_bh(bh);
		ext3_forget(handle, 1, inode, bh, EXT3_I(inode)->i_file_acl);
	} else {
		HDR(bh)->h_refcount = cpu_to_le32(
			le32_to_cpu(HDR(bh)->h_refcount) - 1);
		ext3_journal_dirty_metadata(handle, bh);
		if (IS_SYNC(inode))
			handle->h_sync = 1;
		DQUOT_FREE_BLOCK(inode, 1);
	}
	ea_bdebug(bh, "refcount now=%d", le32_to_cpu(HDR(bh)->h_refcount) - 1);
	unlock_buffer(bh);
	EXT3_I(inode)->i_file_acl = 0;

cleanup:
	brelse(bh);
	up_write(&EXT3_I(inode)->xattr_sem);
}

/*
 * ext3_xattr_put_super()
 *
 * This is called when a file system is unmounted.
 */
void
ext3_xattr_put_super(struct super_block *sb)
{
	mb_cache_shrink(ext3_xattr_cache, sb->s_bdev);
}

/*
 * ext3_xattr_cache_insert()
 *
 * Create a new entry in the extended attribute cache, and insert
 * it unless such an entry is already in the cache.
 *
 * Returns 0, or a negative error number on failure.
 */
static int
ext3_xattr_cache_insert(struct buffer_head *bh)
{
	__u32 hash = le32_to_cpu(HDR(bh)->h_hash);
	struct mb_cache_entry *ce;
	int error;

	ce = mb_cache_entry_alloc(ext3_xattr_cache);
	if (!ce)
		return -ENOMEM;
	error = mb_cache_entry_insert(ce, bh->b_bdev, bh->b_blocknr, &hash);
	if (error) {
		mb_cache_entry_free(ce);
		if (error == -EBUSY) {
			ea_bdebug(bh, "already in cache (%d cache entries)",
				atomic_read(&ext3_xattr_cache->c_entry_count));
			error = 0;
		}
	} else {
		ea_bdebug(bh, "inserting [%x] (%d cache entries)", (int)hash,
			  atomic_read(&ext3_xattr_cache->c_entry_count));
		mb_cache_entry_release(ce);
	}
	return error;
}

/*
 * ext3_xattr_cmp()
 *
 * Compare two extended attribute blocks for equality.
 *
 * Returns 0 if the blocks are equal, 1 if they differ, and
 * a negative error number on errors.
 */
static int
ext3_xattr_cmp(struct ext3_xattr_header *header1,
	       struct ext3_xattr_header *header2)
{
	struct ext3_xattr_entry *entry1, *entry2;

	entry1 = ENTRY(header1+1);
	entry2 = ENTRY(header2+1);
	while (!IS_LAST_ENTRY(entry1)) {
		if (IS_LAST_ENTRY(entry2))
			return 1;
		if (entry1->e_hash != entry2->e_hash ||
		    entry1->e_name_len != entry2->e_name_len ||
		    entry1->e_value_size != entry2->e_value_size ||
		    memcmp(entry1->e_name, entry2->e_name, entry1->e_name_len))
			return 1;
		if (entry1->e_value_block != 0 || entry2->e_value_block != 0)
			return -EIO;
		if (memcmp((char *)header1 + le16_to_cpu(entry1->e_value_offs),
			   (char *)header2 + le16_to_cpu(entry2->e_value_offs),
			   le32_to_cpu(entry1->e_value_size)))
			return 1;

		entry1 = EXT3_XATTR_NEXT(entry1);
		entry2 = EXT3_XATTR_NEXT(entry2);
	}
	if (!IS_LAST_ENTRY(entry2))
		return 1;
	return 0;
}

/*
 * ext3_xattr_cache_find()
 *
 * Find an identical extended attribute block.
 *
 * Returns a pointer to the block found, or NULL if such a block was
 * not found or an error occurred.
 */
static struct buffer_head *
ext3_xattr_cache_find(handle_t *handle, struct inode *inode,
		      struct ext3_xattr_header *header, int *credits)
{
	__u32 hash = le32_to_cpu(header->h_hash);
	struct mb_cache_entry *ce;

	if (!header->h_hash)
		return NULL;  /* never share */
	ea_idebug(inode, "looking for cached blocks [%x]", (int)hash);
	ce = mb_cache_entry_find_first(ext3_xattr_cache, 0,
				       inode->i_sb->s_bdev, hash);
	while (ce) {
		struct buffer_head *bh = sb_bread(inode->i_sb, ce->e_block);

		if (!bh) {
			ext3_error(inode->i_sb, "ext3_xattr_cache_find",
				"inode %ld: block %ld read error",
				inode->i_ino, (unsigned long) ce->e_block);
		} else {
			/* ext3_journal_get_write_access() requires an unlocked
			 * bh, which complicates things here. */
			if (ext3_journal_get_write_access_credits(handle, bh,
								  credits) != 0)
				return NULL;
			lock_buffer(bh);
			if (le32_to_cpu(HDR(bh)->h_refcount) >
				   EXT3_XATTR_REFCOUNT_MAX) {
				ea_idebug(inode, "block %ld refcount %d>%d",
					  (unsigned long) ce->e_block,
					  le32_to_cpu(HDR(bh)->h_refcount),
					  EXT3_XATTR_REFCOUNT_MAX);
			} else if (!ext3_xattr_cmp(header, HDR(bh))) {
				mb_cache_entry_release(ce);
				/* buffer will be unlocked by caller */
				return bh;
			}
			unlock_buffer(bh);
			journal_release_buffer(handle, bh, *credits);
			brelse(bh);
		}
		ce = mb_cache_entry_find_next(ce, 0, inode->i_sb->s_bdev, hash);
	}
	return NULL;
}

/*
 * ext3_xattr_cache_remove()
 *
 * Remove the cache entry of a block from the cache. Called when a
 * block becomes invalid.
 */
static void
ext3_xattr_cache_remove(struct buffer_head *bh)
{
	struct mb_cache_entry *ce;

	ce = mb_cache_entry_get(ext3_xattr_cache, bh->b_bdev,
				bh->b_blocknr);
	if (ce) {
		ea_bdebug(bh, "removing (%d cache entries remaining)",
			  atomic_read(&ext3_xattr_cache->c_entry_count)-1);
		mb_cache_entry_free(ce);
	} else 
		ea_bdebug(bh, "no cache entry");
}

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16

/*
 * ext3_xattr_hash_entry()
 *
 * Compute the hash of an extended attribute.
 */
static inline void ext3_xattr_hash_entry(struct ext3_xattr_header *header,
					 struct ext3_xattr_entry *entry)
{
	__u32 hash = 0;
	char *name = entry->e_name;
	int n;

	for (n=0; n < entry->e_name_len; n++) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - NAME_HASH_SHIFT)) ^
		       *name++;
	}

	if (entry->e_value_block == 0 && entry->e_value_size != 0) {
		__u32 *value = (__u32 *)((char *)header +
			le16_to_cpu(entry->e_value_offs));
		for (n = (le32_to_cpu(entry->e_value_size) +
		     EXT3_XATTR_ROUND) >> EXT3_XATTR_PAD_BITS; n; n--) {
			hash = (hash << VALUE_HASH_SHIFT) ^
			       (hash >> (8*sizeof(hash) - VALUE_HASH_SHIFT)) ^
			       le32_to_cpu(*value++);
		}
	}
	entry->e_hash = cpu_to_le32(hash);
}

#undef NAME_HASH_SHIFT
#undef VALUE_HASH_SHIFT

#define BLOCK_HASH_SHIFT 16

/*
 * ext3_xattr_rehash()
 *
 * Re-compute the extended attribute hash value after an entry has changed.
 */
static void ext3_xattr_rehash(struct ext3_xattr_header *header,
			      struct ext3_xattr_entry *entry)
{
	struct ext3_xattr_entry *here;
	__u32 hash = 0;

	ext3_xattr_hash_entry(header, entry);
	here = ENTRY(header+1);
	while (!IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {
			/* Block is not shared if an entry's hash value == 0 */
			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       le32_to_cpu(here->e_hash);
		here = EXT3_XATTR_NEXT(here);
	}
	header->h_hash = cpu_to_le32(hash);
}

#undef BLOCK_HASH_SHIFT

int __init
init_ext3_xattr(void)
{
	int	err;

	err = ext3_xattr_register(EXT3_XATTR_INDEX_USER,
				  &ext3_xattr_user_handler);
	if (err)
		return err;
	err = ext3_xattr_register(EXT3_XATTR_INDEX_TRUSTED,
				  &ext3_xattr_trusted_handler);
	if (err)
		goto out;
#ifdef CONFIG_EXT3_FS_SECURITY
	err = ext3_xattr_register(EXT3_XATTR_INDEX_SECURITY,
				  &ext3_xattr_security_handler);
	if (err)
		goto out1;
#endif
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	err = init_ext3_acl();
	if (err)
		goto out2;
#endif
	ext3_xattr_cache = mb_cache_create("ext3_xattr", NULL,
		sizeof(struct mb_cache_entry) +
		sizeof(struct mb_cache_entry_index), 1, 6);
	if (!ext3_xattr_cache) {
		err = -ENOMEM;
		goto out3;
	}
	return 0;
out3:
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	exit_ext3_acl();
out2:
#endif
#ifdef CONFIG_EXT3_FS_SECURITY
	ext3_xattr_unregister(EXT3_XATTR_INDEX_SECURITY,
			      &ext3_xattr_security_handler);
out1:
#endif
	ext3_xattr_unregister(EXT3_XATTR_INDEX_TRUSTED,
			      &ext3_xattr_trusted_handler);
out:
	ext3_xattr_unregister(EXT3_XATTR_INDEX_USER,
			      &ext3_xattr_user_handler);
	return err;
}

void
exit_ext3_xattr(void)
{
	if (ext3_xattr_cache)
		mb_cache_destroy(ext3_xattr_cache);
	ext3_xattr_cache = NULL;
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	exit_ext3_acl();
#endif
#ifdef CONFIG_EXT3_FS_SECURITY
	ext3_xattr_unregister(EXT3_XATTR_INDEX_SECURITY,
			      &ext3_xattr_security_handler);
#endif
	ext3_xattr_unregister(EXT3_XATTR_INDEX_TRUSTED,
			      &ext3_xattr_trusted_handler);
	ext3_xattr_unregister(EXT3_XATTR_INDEX_USER,
			      &ext3_xattr_user_handler);
}
