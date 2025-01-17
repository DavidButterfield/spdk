/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * General utility functions
 */

#ifndef SPDK_UTIL_H
#define SPDK_UTIL_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define spdk_min(a,b) (((a)<(b))?(a):(b))
#define spdk_max(a,b) (((a)>(b))?(a):(b))

#define SPDK_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))

#define SPDK_CONTAINEROF(ptr, type, member) ((type *)((uintptr_t)ptr - offsetof(type, member)))

#define SPDK_SEC_TO_USEC 1000000ULL
#define SPDK_SEC_TO_NSEC 1000000000ULL

/* Ceiling division of unsigned integers */
#define SPDK_CEIL_DIV(x,y) (((x)+(y)-1)/(y))

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__))
//XXX Broken! __attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
//XXX SIGSEGV in spdk_u32log2...resolver callecd from _dl_relocate_object on this CPU:
// product: Intel(R) Core(TM) i5-2520M CPU @ 2.50GHz
// version: Intel(R) Core(TM) i5-2520M CPU @ 2.50GHz
// width: 64 bits
// capabilities: x86-64 fpu fpu_exception wp vme de pse tsc msr pae mce cx8 apic
//    sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht
//    tm pbe syscall nx rdtscp constant_tsc arch_perfmon pebs bts rep_good nopl
//    xtopology nonstop_tsc cpuid aperfmperf pni pclmulqdq dtes64 monitor ds_cpl
//    vmx smx est tm2 ssse3 cx16 xtpr pdcm pcid sse4_1 sse4_2 x2apic popcnt
//    tsc_deadline_timer aes xsave avx lahf_lm epb pti ssbd ibrs ibpb stibp
//    tpr_shadow vnmi flexpriority ept vpid xsaveopt dtherm ida arat pln pts
//    md_clear flush_l1d cpufreq
// configuration: cores=2 enabledcores=2 threads=4
#endif
static inline uint32_t
spdk_u32log2(uint32_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		return 0;
	}
	return 31u - __builtin_clz(x);
}

static inline uint32_t
spdk_align32pow2(uint32_t x)
{
	return 1u << (1 + spdk_u32log2(x - 1));
}

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__))
//XXX Broken! __attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
#endif
static inline uint64_t
spdk_u64log2(uint64_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		return 0;
	}
	return 63u - __builtin_clzl(x);
}

static inline uint64_t
spdk_align64pow2(uint64_t x)
{
	return 1u << (1 + spdk_u64log2(x - 1));
}

/**
 * Check if a uint32_t is a power of 2.
 */
static inline bool
spdk_u32_is_pow2(uint32_t x)
{
	if (x == 0) {
		return false;
	}

	return (x & (x - 1)) == 0;
}

static inline uint64_t
spdk_divide_round_up(uint64_t num, uint64_t divisor)
{
	return (num + divisor - 1) / divisor;
}


/**
 * Scan build is really pessimistic and assumes that mempool functions can
 * dequeue NULL buffers even if they return success. This is obviously a false
 * possitive, but the mempool dequeue can be done in a DPDK inline function that
 * we can't decorate with usual assert(buf != NULL). Instead, we'll
 * preinitialize the dequeued buffer array with some dummy objects.
 */
#define SPDK_CLANG_ANALYZER_PREINIT_PTR_ARRAY(arr, arr_size, buf_size) \
	do { \
		static char dummy_buf[buf_size]; \
		int i; \
		for (i = 0; i < arr_size; i++) { \
			arr[i] = (void *)dummy_buf; \
		} \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif
