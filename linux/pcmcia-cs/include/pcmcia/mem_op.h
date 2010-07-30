/*
 * mem_op.h 1.14 2001/08/24 12:16:13
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License
 * at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and
 * limitations under the License. 
 *
 * The initial developer of the original code is David A. Hinds
 * <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
 * are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License version 2 (the "GPL"), in
 * which case the provisions of the GPL are applicable instead of the
 * above.  If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use
 * your version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GPL.  If you do not delete the
 * provisions above, a recipient may use your version of this file
 * under either the MPL or the GPL.
 */

#ifndef _LINUX_MEM_OP_H
#define _LINUX_MEM_OP_H

#include <asm/uaccess.h>

/*
   If UNSAFE_MEMCPY is defined, we use the (optimized) system routines
   to copy between a card and kernel memory.  These routines do 32-bit
   operations which may not work with all PCMCIA controllers.  The
   safe versions defined here will do only 8-bit and 16-bit accesses.
*/

#ifdef UNSAFE_MEMCPY

#define copy_from_pc memcpy_fromio
#define copy_to_pc memcpy_toio

static inline void copy_pc_to_user(void *to, const void *from, size_t n)
{
    size_t odd = (n & 3);
    n -= odd;
    while (n) {
	put_user(readl_ns(from), (int *)to);
	from += 4; to += 4; n -= 4;
    }
    while (odd--)
	put_user(readb((char *)from++), (char *)to++);
}

static inline void copy_user_to_pc(void *to, const void *from, size_t n)
{
    int l;
    char c;
    size_t odd = (n & 3);
    n -= odd;
    while (n) {
        l = get_user((int *)from);
	writel_ns(l, to);
	to += 4; from += 4; n -= 4;
    }
    while (odd--) {
	c = get_user((char *)from++);
	writeb(c, (char *)to++);
    }
}

#else /* UNSAFE_MEMCPY */

static inline void copy_from_pc(void *to, const void *from, size_t n)
{
    size_t odd = (n & 1);
    n -= odd;
    while (n) {
	*(u_short *)to = readw_ns(from);
	to += 2; from += 2; n -= 2;
    }
    if (odd)
	*(u_char *)to = readb(from);
}

static inline void copy_to_pc(void *to, const void *from, size_t n)
{
    size_t odd = (n & 1);
    n -= odd;
    while (n) {
	writew_ns(*(u_short *)from, to);
	to += 2; from += 2; n -= 2;
    }
    if (odd)
	writeb(*(u_char *)from, to);
}

static inline void copy_pc_to_user(void *to, const void *from, size_t n)
{
    size_t odd = (n & 1);
    n -= odd;
    while (n) {
	put_user(readw_ns(from), (short *)to);
	to += 2; from += 2; n -= 2;
    }
    if (odd)
	put_user(readb(from), (char *)to);
}

static inline void copy_user_to_pc(void *to, const void *from, size_t n)
{
    short s;
    char c;
    size_t odd = (n & 1);
    n -= odd;
    while (n) {
	s = get_user((short *)from);
	writew_ns(s, to);
	to += 2; from += 2; n -= 2;
    }
    if (odd) {
	c = get_user((char *)from);
	writeb(c, to);
    }
}

#endif /* UNSAFE_MEMCPY */

#endif /* _LINUX_MEM_OP_H */
