/****************************************************************************
 * libs/libc/string/lib_memmem.c
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

#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LONG_INT_N_BYTES sizeof(long)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: memmem
 *
 * Description:
 *   The memmem() function finds the start of the first occurrence of the
 *   substring needle of length needlelen in the memory area haystack of
 *   length haystacklen.
 *
 * Returned Value:
 *   The memmem() function returns a pointer to the beginning of the
 *   substring, or NULL if the substring is not found.
 *
 ****************************************************************************/

#undef memmem /* See mm/README.txt */
FAR void *memmem(FAR const void *haystack, size_t haystacklen,
                 FAR const void *needle, size_t needlelen)
{
    FAR const unsigned char *needle_ptr;
    FAR const unsigned char *haystack_ptr;
    int sums_diff;
    size_t compare_len;
    unsigned long last_needle_chars;
    unsigned long last_haystack_chars;
    unsigned int i;

    switch (needlelen)
      {
        case (0):

            /* empty needle */

            return (FAR void *)haystack;
            break;
        case (1):

            /* special case for single-character needles */

            return memchr(haystack,
                          *((FAR unsigned char *)needle), haystacklen);
            break;
      }

    /* start searching through haystack only from the first occurence of
     * the first character of needle.
     */

    haystack_ptr = (FAR const unsigned char *)memchr(haystack,
                    *((FAR const unsigned char *)needle), haystacklen);
    if (!haystack_ptr)
      {
        /* the first character of needle isn't in haystack */

        return NULL;
      }

    haystacklen -= (haystack_ptr - (FAR const unsigned char *)haystack);
    if (haystacklen < needlelen)
      {
        /* the remaining haystack is smaller than needle */

        return NULL;
      }

    haystack = (FAR void *)haystack_ptr;

    if (needlelen > LONG_INT_N_BYTES + 1)
      {
        needle_ptr = (FAR const unsigned char *)needle;
        sums_diff = 0;
        for (i = needlelen - LONG_INT_N_BYTES; i > 0; --i)
          {
            sums_diff -= *needle_ptr++;
            sums_diff += *haystack_ptr++;
          }

        last_needle_chars = 0;
        last_haystack_chars = 0;
        for (i = LONG_INT_N_BYTES; i > 0; --i)
          {
            last_needle_chars <<= 8;
            last_needle_chars ^= *needle_ptr;
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr;
            sums_diff -= *needle_ptr++;
            sums_diff += *haystack_ptr++;
          }

        /* we will call memcmp() only once we know that the sums are equal
         * and that LONG_INT_N_BYTES last chars are equal, so it will be
         * enough to compare all but the last LONG_INT_N_BYTES + 1
         * characters.
         */

        compare_len = needlelen - (LONG_INT_N_BYTES + 1);

        /* At this point:
         * needle is at least two characters long
         * haystack is at least needlelen characters long (also at least two)
         * the first characters of needle and haystack are identical
         */

        if (sums_diff == 0
            && last_haystack_chars == last_needle_chars
            && memcmp(haystack, needle, compare_len) == 0)
          {
            return (FAR void *)haystack;
          }

        /* iterate through the remainder of haystack, updating the sums'
         * differenceand checking for identity whenever the difference
         * is zero.
         */

        for (i = haystacklen - needlelen; i > 0; --i)
          {
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr;
            sums_diff -= *(FAR const unsigned char *)haystack++;
            sums_diff += *haystack_ptr++;
            /* if sums_diff == 0, we know that the sums are equal, so it is
             * enough to compare all but the last characters.
             */

            if (sums_diff == 0
                && last_haystack_chars == last_needle_chars
                && memcmp(haystack, needle, compare_len) == 0)
              {
                return (FAR void *)haystack;
              }
          }
      }
    else if (needlelen < LONG_INT_N_BYTES)
      {
        needle_ptr = (FAR const unsigned char *)needle;
        sums_diff = 0;
        for (i = needlelen; i > 0; --i)
          {
            sums_diff -= *needle_ptr++;
            sums_diff += *haystack_ptr++;
          }

        /* we will call memcmp() only once we know that the sums are equal,
         * so it will be enough to compare all but the last characters.
         */

        compare_len = needlelen - 1;

        /* At this point:
         * needle is at least two characters long
         * haystack is at least needlelen characters long (also at least two)
         * the first characters of needle and haystack are identical
         */

        if (sums_diff == 0
            && memcmp(haystack, needle, compare_len) == 0)
          {
            return (FAR void *)haystack;
          }

        /* iterate through the remainder of haystack, updating the sums'
         * difference and checking for identity whenever the difference
         * is zero.
         */

        for (i = haystacklen - needlelen; i > 0; --i)
          {
            sums_diff -= *(FAR const unsigned char *)haystack++;
            sums_diff += *haystack_ptr++;
            /* if sums_diff == 0, we know that the sums are equal, so it is
             * enough to compare all but the last characters.
             */

            if (sums_diff == 0
                && memcmp(haystack, needle, compare_len) == 0)
              {
                return (FAR void *)haystack;
              }
          }
      }
    else if (needlelen == LONG_INT_N_BYTES)
      {
        needle_ptr = (FAR const unsigned char *)needle;
        last_needle_chars = 0;
        last_haystack_chars = 0;
        for (i = needlelen; i > 0; --i)
          {
            last_needle_chars <<= 8;
            last_needle_chars ^= *needle_ptr++;
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr++;
          }

        if (last_haystack_chars == last_needle_chars)
          {
            return (FAR void *)haystack;
          }

        /* iterate through the remainder of haystack, updating the last char
         * data and checking for equality.
         */

        for (i = haystacklen - needlelen; i > 0; --i)
          {
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr++;
            if (last_haystack_chars == last_needle_chars)
              {
                return (FAR void *)(haystack_ptr - needlelen);
              }
          }
      }
    else /* needlelen == LONG_INT_N_BYTES + 1 */
      {
        needle_ptr = (FAR const unsigned char *)needle;
        last_needle_chars = 0;
        last_haystack_chars = 0;
        for (i = LONG_INT_N_BYTES; i > 0; --i)
          {
            last_needle_chars <<= 8;
            last_needle_chars ^= *needle_ptr++;
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr++;
          }

        unsigned char last_needle_char =
                *(((FAR const unsigned char *)needle) + LONG_INT_N_BYTES);

        if (last_haystack_chars == last_needle_chars
            && *haystack_ptr == last_needle_char)
          {
            return (FAR void *)haystack;
          }

        /* iterate through the remainder of haystack, updating the last char
         * data and checking for equality.
         */

        for (i = haystacklen - needlelen; i > 0; --i)
          {
            last_haystack_chars <<= 8;
            last_haystack_chars ^= *haystack_ptr++;
            if (last_haystack_chars == last_needle_chars
                && *haystack_ptr == last_needle_char)
              {
                return (FAR void *)(haystack_ptr - (needlelen - 1));
              }
          }
      }

    return NULL;
}
