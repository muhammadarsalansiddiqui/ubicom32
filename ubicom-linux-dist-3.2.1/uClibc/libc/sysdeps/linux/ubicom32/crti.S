/*
Copyright (C) 2009, Ubicom, Inc.
Copyright (C) 2003, 2004 Free Software Foundation, Inc.
This file is part of the GNU C Library.

The GNU C Library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation; either version 2.1 of the
License, or (at your option) any later version.

In addition to the permissions in the GNU Lesser General Public
License, the Free Software Foundation gives you unlimited
permission to link the compiled version of this file with other
programs, and to distribute those programs without any restriction
coming from the use of this file.  (The GNU Lesser General Public
License restrictions do apply in other respects; for example, they
cover modification of the file, and distribution when not linked
into another program.)

The GNU C Library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with the GNU C Library; see the file COPYING.LIB.  If
not, write to the Free Software Foundation, Inc., 675 Mass Ave,
Cambridge, MA 02139, USA.  */

/*
 * This file just supplies function prologues for the .init and .fini
 * sections.  It is linked in before crtbegin.o.
 */
	.file   "crti.o"
	.ident  "GNU C crti.o"

	.section .init
	.align	2
	.globl	_init
	.type	_init, @function
_init:
	move.4 -4(sp)++, a5
#ifdef __UBICOM32_FDPIC__
	move.4 -4(sp)++, a0
#endif

	.section .fini
	.align	2
	.globl	_fini
	.type	_fini, @function
_fini:
	move.4 -4(sp)++, a5
#ifdef __UBICOM32_FDPIC__
	move.4 -4(sp)++, a0
#endif
