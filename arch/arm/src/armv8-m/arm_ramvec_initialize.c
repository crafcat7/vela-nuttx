/****************************************************************************
 * arch/arm/src/armv8-m/arm_ramvec_initialize.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "nvic.h"
#include "ram_vectors.h"

#include "chip.h"             /* May redefine VECTAB fields */
#include "arm_internal.h"

#ifdef CONFIG_ARCH_RAMVECTORS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* If CONFIG_ARCH_RAMVECTORS is defined, then the ARM logic must provide
 * ARM-specific implementations of arm_ramvec_initialize(), irq_attach(), and
 * irq_dispatch.  In this case, it is also assumed that the ARM vector
 * table resides in RAM, has the name g_ram_vectors, and has been
 * properly positioned and aligned in memory by the linker script.
 *
 * REVISIT: Can this alignment requirement vary from core-to-core?  Yes, it
 * depends on the number of vectors supported by the MCU. The safest thing
 * to do is to put the vector table at the beginning of RAM in order to force
 * the highest alignment possible.
 */

up_vector_t g_ram_vectors[ARMV8M_VECTAB_SIZE]
  locate_data(".ram_vectors") aligned_data(VECTAB_ALIGN);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_ramvec_initialize
 *
 * Description:
 *   Copy vectors to RAM an configure the NVIC to use the RAM vectors.
 *
 ****************************************************************************/

void arm_ramvec_initialize(void)
{
  const up_vector_t *src;
  up_vector_t *dest;
  int i;

  /* The vector table must be aligned */

  DEBUGASSERT(((uint32_t)g_ram_vectors & ~NVIC_VECTAB_TBLOFF_MASK) == 0);

  /* Copy the ROM vector table at address zero to RAM vector table.
   *
   * This must be done BEFORE the MPU is enable if the MPU is being used to
   * protect against NULL pointer references.
   */

  src  = (const up_vector_t *)getreg32(NVIC_VECTAB);
  dest = g_ram_vectors;

  irqinfo("src=%p dest=%p\n", src, dest);

  for (i = 0; i < ARMV8M_VECTAB_SIZE; i++)
    {
      *dest++ = *src++;
    }

  /* Now configure the NVIC to use the new vector table. */

  putreg32((uint32_t)g_ram_vectors, NVIC_VECTAB);

  /* The number bits required to align the RAM vector table seem to vary
   * from part-to-part.  The following assertion will catch the case where
   * the table alignment is insufficient.
   */

  irqinfo("NVIC_VECTAB=%08" PRIx32 "\n", getreg32(NVIC_VECTAB));
  DEBUGASSERT(getreg32(NVIC_VECTAB) == (uint32_t)g_ram_vectors);
}

#endif /* !CONFIG_ARCH_RAMVECTORS */
