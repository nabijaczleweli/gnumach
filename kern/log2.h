/*
 * Copyright (c) 2014 Richard Braun.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Integer base 2 logarithm operations.
 */

#ifndef _KERN_LOG2_H
#define _KERN_LOG2_H

#include <kern/assert.h>

#ifdef __LP64__
#define LONG_BIT 64
#else /* __LP64__ */
#define LONG_BIT 32
#endif /* __LP64__ */

static inline unsigned int
ilog2(unsigned long x)
{
    assert(x != 0);
    return LONG_BIT - __builtin_clzl(x) - 1;
}

static inline unsigned int
iorder2(unsigned long size)
{
    assert(size != 0);

    if (size == 1)
        return 0;

    return ilog2(size - 1) + 1;
}

#endif /* _KERN_LOG2_H */
