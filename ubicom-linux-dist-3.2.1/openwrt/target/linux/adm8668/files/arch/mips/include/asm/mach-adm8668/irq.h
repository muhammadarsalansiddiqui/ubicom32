/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2003 by Ralf Baechle
 */
#ifndef __ASM_MACH_ADM8668_IRQ_H
#define __ASM_MACH_ADM8668_IRQ_H

#ifndef NR_IRQS
#define NR_IRQS	32
#endif

#ifdef CONFIG_IRQ_CPU

#ifndef MIPS_CPU_IRQ_BASE
#define MIPS_CPU_IRQ_BASE 16
#endif

#ifdef CONFIG_IRQ_CPU_RM7K
#ifndef RM7K_CPU_IRQ_BASE
#define RM7K_CPU_IRQ_BASE (MIPS_CPU_IRQ_BASE+8)
#endif
#endif

#ifdef CONFIG_IRQ_CPU_RM9K
#ifndef RM9K_CPU_IRQ_BASE
#define RM9K_CPU_IRQ_BASE (MIPS_CPU_IRQ_BASE+12)
#endif
#endif

#endif /* CONFIG_IRQ_CPU */

#endif /* __ASM_MACH_ADM8668_IRQ_H */
