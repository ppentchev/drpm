/*
    Authors:
        Matej Chalk <mchalk@redhat.com>

    Copyright (C) 2015 Red Hat

    This program is licensed under the BSD license, read LICENSE.BSD
    for further information.
*/

/*
  Copyright 2004,2005 Michael Schroeder

  rewritten from bsdiff.c,
      http://www.freebsd.org/cgi/cvsweb.cgi/src/usr.bin/bsdiff
  added library interface and hash method, enhanced suffix method.
*/
/*-
 * Copyright 2003-2005 Colin Percival
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "drpm.h"
#include "drpm_private.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MIN_MISMATCHES 32

static size_t match_len(const unsigned char *, size_t, const unsigned char *, size_t);
static uint32_t buzhash(const unsigned char *);
static int bucketsort(long long *, long long *, size_t, size_t);
static void suffix_split(long long *, long long *, size_t, size_t, size_t);
static size_t suffix_search(const long long *, const unsigned char *, size_t,
                            const unsigned char *, size_t, size_t, size_t, size_t *);

size_t match_len(const unsigned char *old, size_t old_len,
                 const unsigned char *new, size_t new_len)
{
    size_t i;
    size_t len = MIN(old_len, new_len);

    for (i = 0; i < len; i++)
        if (old[i] != new[i])
            break;

    return i;
}

/********************************* hash *********************************/

#define HSIZESHIFT 4
#define HSIZE (1 << HSIZESHIFT)

struct hash {
    size_t *hash_table;
    size_t ht_len;
};

/* 256 random numbers generated by a quantum source */
static const uint32_t noise[256] =
{
    0x9BE502A4, 0xBA7180EA, 0x324E474F, 0x0AAB8451, 0x0CED3810,
    0x2158A968, 0x6BBD3771, 0x75A02529, 0x41F05C14, 0xC2264B87,
    0x1F67B359, 0xCD2D031D, 0x49DC0C04, 0xA04AE45C, 0x6ADE28A7,
    0x2D0254FF, 0xDEC60C7C, 0xDEF5C084, 0x0F77FFC8, 0x112021F6,
    0x5F6D581E, 0xE35EA3DF, 0x3216BFB4, 0xD5A3083D, 0x7E63E9CD,
    0xAA9208F6, 0xDA3F3978, 0xFE0E2547, 0x09DFB020, 0xD97472C5,
    0xBBCE2EDE, 0x121AEBD2, 0x0E9FDBEB, 0x7B6F5D9C, 0x84938E43,
    0x30694F2D, 0x86B7A7F8, 0xEFAF5876, 0x263812E6, 0xB6E48DDF,
    0xCE8ED980, 0x4DF591E1, 0x75257B35, 0x2F88DCFF, 0xA461FE44,
    0xCA613B4D, 0xD9803F73, 0xEA056205, 0xCCCA7A89, 0x0F2DBB07,
    0xC53E359E, 0xE80D0137, 0x2B2D2A5D, 0xCFC1391A, 0x2BB3B6C5,
    0xB66AEA3C, 0x00EA419E, 0xCE5ADA84, 0xAE1D6712, 0x12F576BA,
    0x117FCBC4, 0xA9D4C775, 0x25B3D616, 0xEFDA65A8, 0xAFF3EF5B,
    0x00627E68, 0x668D1E99, 0x088D0EEF, 0xF8FAC24D, 0xE77457C7,
    0x68D3BEB4, 0x921D2ACB, 0x9410EAC9, 0xD7F24399, 0xCBDEC497,
    0x98C99AE1, 0x65802B2C, 0x81E1C3C4, 0xA130BB09, 0x17A87BAD,
    0xA70367D6, 0x148658D4, 0x02F33377, 0x8620D8B6, 0xBDAC25BD,
    0xB0A6DE51, 0xD64C4571, 0xA4185BA0, 0xA342D70F, 0x3F1DC4C1,
    0x042DC3CE, 0x0DE89F43, 0xA69B1867, 0x3C064E11, 0xAD1E2C3E,
    0x9660E8CD, 0xD36B09CA, 0x4888F228, 0x61A9AC3C, 0xD9561118,
    0x3532797E, 0x71A35C22, 0xECC1376C, 0xAB31E656, 0x88BD0D35,
    0x423B20DD, 0x38E4651C, 0x3C6397A4, 0x4A7B12D9, 0x08B1CF33,
    0xD0604137, 0xB035FDB8, 0x4916DA23, 0xA9349493, 0xD83DAA9B,
    0x145F7D95, 0x868531D6, 0xACB18F17, 0x9CD33B6F, 0x193E42B9,
    0x26DFDC42, 0x5069D8FA, 0x5BEE24EE, 0x5475D4C6, 0x315B2C0C,
    0xF764EF45, 0x01B6F4EB, 0x60BA3225, 0x8A16777C, 0x4C05CD28,
    0x53E8C1D2, 0xC8A76CE5, 0x8045C1E6, 0x61328752, 0x2EBAD322,
    0x3444F3E2, 0x91B8AF11, 0xB0CEE675, 0x55DBFF5A, 0xF7061EE0,
    0x27D7D639, 0xA4AEF8C9, 0x42FF0E4F, 0x62755468, 0x1C6CA3F3,
    0xE4F522D1, 0x2765FCB3, 0xE20C8A95, 0x3A69AEA7, 0x56AB2C4F,
    0x8551E688, 0xE0BC14C2, 0x278676BF, 0x893B6102, 0xB4F0AB3B,
    0xB55DDDA9, 0xA04C521F, 0xC980088E, 0x912AEAC1, 0x08519BAD,
    0x991302D3, 0x5B91A25B, 0x696D9854, 0x9AD8B4BF, 0x41CB7E21,
    0xA65D1E03, 0x85791D29, 0x89478AA7, 0x4581E337, 0x59BAE0B1,
    0xE0FC9DF3, 0x45D9002C, 0x7837464F, 0xDA22DE3A, 0x1DC544BD,
    0x601D8BAD, 0x668B0ABC, 0x7A5EBFB1, 0x3AC0B624, 0x5EE16D7D,
    0x9BFAC387, 0xBE8EF20C, 0x8D2AE384, 0x819DC7D5, 0x7C4951E7,
    0xE60DA716, 0x0C5B0073, 0xB43B3D97, 0xCE9974ED, 0x0F691DA9,
    0x4B616D60, 0x8FA9E819, 0x3F390333, 0x6F62FAD6, 0x5A32B67C,
    0x3BE6F1C3, 0x05851103, 0xFF28828D, 0xAA43A56A, 0x075D7DD5,
    0x248C4B7E, 0x52FDE3EB, 0xF72E2EDA, 0x5DA6F75F, 0x2F5148D9,
    0xCAE2AEAE, 0xFDA6F3E5, 0xFF60D8FF, 0x2ADC02D2, 0x1DBDBD4C,
    0xD410AD7C, 0x8C284AAE, 0x392EF8E0, 0x37D48B3A, 0x6792FE9D,
    0xAD32DDFA, 0x1545F24E, 0x3A260F73, 0xB724CA36, 0xC510D751,
    0x4F8DF992, 0x000B8B37, 0x292E9B3D, 0xA32F250F, 0x8263D144,
    0xFCAE0516, 0x1EAE2183, 0xD4AF2027, 0xC64AFAE3, 0xE7B34FE4,
    0xDF864AEA, 0x80CC71C5, 0x0E814DF3, 0x66CC5F41, 0x853A497A,
    0xA2886213, 0x5E34A2EA, 0x0F53BA47, 0x718C484A, 0xFA0F0B12,
    0x33CC59FF, 0x72B48E07, 0x8B6F57BC, 0x29CF886D, 0x1950955B,
    0xCD52910C, 0x4CECEF65, 0x05C2CBFE, 0x49DF4F6A, 0x1F4C3F34,
    0xFADC1A09, 0xF2D65A24, 0x117F5594, 0xDE3A84E6, 0x48DB3024,
    0xD10CA9B5
};

/* buzhash by Robert C. Uzgalis
 * General hash functions. Technical Report TR-92-01,
 * The University of Hong Kong, 1993 */
uint32_t buzhash(const unsigned char *buf)
{
    uint32_t x = 0x83D31DF4;

    for (unsigned i = 0; i < HSIZE; i++)
        x = (x << 1) ^ (x & (1 << 31) ? 1 : 0) ^ noise[*buf++];

    return x;
}

int hash_create(struct hash **hsh, const unsigned char *old, size_t old_len)
{
    size_t *hash_table;
    size_t ht_len;
    size_t key;
    const unsigned char * const old_ptr = old;
    size_t primes[] = {
        65537, 98317, 147481, 221227, 331841, 497771, 746659, 1120001,
        1680013, 2520031, 3780053, 5670089, 8505137, 12757739, 19136609,
        28704913, 43057369, 64586087, 96879131, 145318741, 217978121,
        326967209, 490450837, 735676303, 1103514463, 1655271719,
        0xFFFFFFFF
    };
    size_t i;
    const size_t i_limit = sizeof(primes)/sizeof(*primes) - 1;

    if ((*hsh = malloc(sizeof(struct hash))) == NULL)
        return DRPM_ERR_MEMORY;

    ht_len = 4 * ((old_len + HSIZE - 1) >> HSIZESHIFT);
    for (i = 0; i < i_limit; i++) {
        if (ht_len < primes[i])
            break;
    }
    ht_len = primes[i];

    if ((hash_table = calloc(ht_len, sizeof(size_t))) == NULL) {
        free(*hsh);
        return DRPM_ERR_MEMORY;
    }

    for (size_t off = 0; old_len >= HSIZE; off += HSIZE, old += HSIZE, old_len -= HSIZE) {
        key = buzhash(old) % ht_len;
        if (hash_table[key]) {
            if (hash_table[(key == ht_len - 1) ? 0 : key + 1])
                continue;
            if (memcmp(old, old_ptr + hash_table[key], HSIZE) == 0)
                continue;
            key = (key == ht_len - 1) ? 0 : key + 1;
        }
        hash_table[key] = off + 1;
    }

    (*hsh)->hash_table = hash_table;
    (*hsh)->ht_len = ht_len;

    return DRPM_ERR_OK;
}

void hash_free(struct hash **hsh)
{
    free((*hsh)->hash_table);
    free(*hsh);
}

size_t hash_search(struct hash *hsh,
                   const unsigned char *old, size_t old_len,
                   const unsigned char *new, size_t new_len,
                   size_t last_offset, size_t scan,
                   size_t *pos_ret, size_t *len_ret)
{
    size_t *hash_table = hsh->hash_table;
    size_t ht_len = hsh->ht_len;

    size_t last_scan = 0;
    size_t last_pos = 0;
    size_t last_len = 0;

    size_t miniscan;
    size_t scan_start = scan;
    size_t old_score = 0;
    size_t old_score_num = 0;
    size_t old_score_start = 0;

    size_t pos = 0;
    size_t len = 0;
    size_t pos2;
    size_t len2;

    uint32_t key;
    uint32_t key2;
    uint32_t prekey = (scan <= new_len - HSIZE) ? buzhash(new + scan) : 0;
    uint32_t xprekey;

    hash_table = hsh->hash_table;
    ht_len = hsh->ht_len;
    scan_start = scan;
    old_score = old_score_num = old_score_start = 0;
    prekey = (scan <= new_len - HSIZE) ? buzhash(new + scan) : 0;
    pos = 0;
    len = 0;
    last_pos = last_scan = last_len = 0;

    while (true) {
        if (scan >= new_len - HSIZE) {
            if (last_len >= 32)
                goto gotit;
            break;
        }

        key = prekey % ht_len;
        pos = hash_table[key];

        if (pos == 0) {
scannext:
            if (last_len >= 32 && scan - last_scan >= HSIZE)
                goto gotit;
            prekey = (prekey << 1) ^ (prekey & (1 << 31) ? 1 : 0) ^ noise[new[scan + HSIZE]];
            xprekey = noise[new[scan]] ^ (0x83D31DF4 ^ 0x07A63BE9);
#if HSIZE % 32 != 0
            prekey ^= (xprekey << (HSIZE % 32)) ^ (xprekey >> (32 - (HSIZE % 32)));
#else
            prekey ^= xprekey;
#endif
            scan++;
            continue;
        }
        pos--;
        if (memcmp(old + pos, new + scan, HSIZE) != 0) {
            pos = hash_table[key + 1 == ht_len ? 0 : key + 1];
            if (pos == 0)
                goto scannext;
            pos--;
            if (memcmp(old + pos, new + scan, HSIZE) != 0)
                goto scannext;
        }
        len = match_len(old + pos + HSIZE, old_len - pos - HSIZE, new + scan + HSIZE, new_len - scan - HSIZE) + HSIZE;
        if (scan + HSIZE * 4 <= new_len) {
            key2 = buzhash(new + scan + 3 * HSIZE) % ht_len;
            pos2 = hash_table[key2];
            if (pos2) {
                if (memcmp(new + scan + 3 * HSIZE, old + pos2 - 1, HSIZE) != 0) {
                    key2 = (key2 == ht_len - 1) ? 0 : key2 + 1;
                    pos2 = hash_table[key2];
                }
            }
            if (pos2 > 1 + 3 * HSIZE) {
                pos2 -= 1 + 3 * HSIZE;
                if (pos2 != pos) {
                    len2 = match_len(old + pos2, old_len - pos2, new + scan, new_len - scan);
                    if (len2 > len) {
                        pos = pos2;
                        len = len2;
                    }
                }
            }
        }
        if (len > last_len) {
            last_len = len;
            last_pos = pos;
            last_scan = scan;
        }
        goto scannext;

gotit:
        scan = last_scan;
        len = last_len;
        pos = last_pos;
        if (scan + last_offset == pos) {
            scan += len;
            scan_start = scan;
            if (scan + HSIZE < new_len)
                prekey = buzhash(new + scan);
            last_len = 0;
            continue;
        }
        for (size_t i = scan - scan_start; i > 0 && pos > 0 && scan > 0 && old[pos - 1] == new[scan - 1]; i--) {
            len++;
            pos--;
            scan--;
        }
        if (old_score_start + 1 != scan || old_score_num == 0 || old_score_num - 1 > len) {
            old_score = 0;
            for (miniscan = scan; miniscan < scan + len; miniscan++)
                if ((miniscan + last_offset < old_len) && (old[miniscan + last_offset] == new[miniscan]))
                    old_score++;
            old_score_start = scan;
            old_score_num = len;
        } else {
            if (old_score_start + last_offset < old_len && old[old_score_start + last_offset] == new[old_score_start])
                old_score--;
            old_score_start++;
            old_score_num--;
            for (miniscan = old_score_start + old_score_num; old_score_num < len; miniscan++) {
                if ((miniscan + last_offset < old_len) && (old[miniscan + last_offset] == new[miniscan]))
                    old_score++;
                old_score_num++;
            }
        }

        if (len - old_score >= 32)
            break;

        if (len > 3 * HSIZE + 32)
            scan += len - (3 * HSIZE + 32);
        if (scan <= last_scan)
            scan = last_scan + 1;
        scan_start = scan;
        if (scan + HSIZE < new_len)
            prekey = buzhash(new + scan);
        last_len = 0;
    }

    if (scan >= new_len - HSIZE) {
      scan = new_len;
      pos = 0;
      len = 0;
    }

    *pos_ret = pos;
    *len_ret = len;

    return scan;
}

/**************************** suffix sort ****************************/

struct sfxsrt {
    long long *I;   // suffix array
    size_t F[257];  // key = byte value,
                    // value = where to start looking in suffix array
};

int sfxsrt_create(struct sfxsrt **suf, const unsigned char *old, size_t old_len)
{
    int error = DRPM_ERR_OK;
    long long *I = NULL;
    long long *V = NULL;
    size_t h;
    size_t bucket_len;
    size_t len;
    size_t val;
    size_t l;
    size_t i;
    uint32_t oldv;
    size_t F[257] = {0};

    if (suf == NULL || old == NULL)
        return DRPM_ERR_PROG;

    if ((*suf = malloc(sizeof(struct sfxsrt))) == NULL ||
        (I = calloc((old_len + 3), sizeof(long long))) == NULL ||
        (V = calloc((old_len + 3), sizeof(long long))) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup_fail;
    }

    if (old_len > 0xFFFFFF) {
        bucket_len = 0x1000002;
        h = 3;

        F[old[0]]++;
        F[old[1]]++;
        oldv = old[0] << 8 | old[1];
        for (size_t i = 2; i < old_len; i++) {
            F[old[i]]++;
            oldv = (oldv & 0xFFFF) << 8 | old[i];
            V[i - 2] = oldv + 2;
        }
        oldv = (oldv & 0xFFFF) << 8;
        V[old_len - 2] = oldv + 2;
        oldv = (oldv & 0xFFFF) << 8;
        V[old_len - 1] = oldv + 2;
        len = old_len + 2;
        V[len - 2] = 1;
        V[len - 1] = 0;
    } else {
        bucket_len = 0x10001;
        h = 2;

        F[old[0]]++;
        oldv = old[0];
        for (size_t i = 1; i < old_len; i++) {
            F[old[i]]++;
            oldv = (oldv & 0xFF) << 8 | old[i];
            V[i - 1] = oldv + 1;
        }
        oldv = (oldv & 0xFF) << 8;
        V[old_len - 1] = oldv + 1;
        len = old_len + 1;
        V[len - 1] = 0;
    }

    val = len;
    for (unsigned short i = 256; i > 0; val -= F[--i])
        F[i] = val;
    F[0] = val;

    if ((error = bucketsort(I, V, len, bucket_len)) != DRPM_ERR_OK)
        goto cleanup_fail;

    len++;
    for ( ; I[0] != -(long long)len; h += h) {
        l = 0;
        for (i = 0; i < len; ) {
            if (I[i] < 0) {
                l -= I[i];
                i -= I[i];
            } else {
                if (l > 0)
                    I[i - l] = -(long long)l;
                l = V[I[i]] + 1 - i;
                suffix_split(I, V, i, l, h);
                i += l;
                l = 0;
            }
        }
        if (l > 0)
            I[i - l] = -(long long)l;
    }

    for (i = 0; i < len; i++)
        I[V[i]] = i;

    (*suf)->I = I;
    memcpy((*suf)->F, F, sizeof(size_t) * 257);

    goto cleanup;

cleanup_fail:
    free(*suf);
    free(I);

cleanup:
    free(V);

    return error;
}

void sfxsrt_free(struct sfxsrt **suf)
{
    free((*suf)->I);
    free(*suf);
}

size_t sfxsrt_search(struct sfxsrt *suf,
                     const unsigned char *old, size_t old_len,
                     const unsigned char *new, size_t new_len,
                     size_t last_offset, size_t scan,
                     size_t *pos_ret, size_t *len_ret)
{
    size_t len = 0;
    size_t old_score = 0;
    size_t miniscan = scan;

    *pos_ret = 0;

    while (scan < new_len) {
        len = suffix_search(suf->I, old, old_len, new + scan, new_len - scan,
                            suf->F[new[scan]] + 1, suf->F[new[scan] + 1],
                            pos_ret);

        const size_t miniscan_limit = MIN(scan + len, old_len - last_offset);
        for ( ; miniscan < miniscan_limit; miniscan++)
            if (old[miniscan + last_offset] == new[miniscan])
                old_score++;
        miniscan = scan + len;

        if (len > 0 && len == old_score) {
            scan += len;
            miniscan = scan;
            old_score = 0;
            continue;
        }

        if (len - old_score > MIN_MISMATCHES)
            break;

        if (scan + last_offset < old_len &&
            old[scan + last_offset] == new[scan])
            old_score--;

        scan++;
    }

    *len_ret = len;

    return scan;
}

int bucketsort(long long *I, long long *V, size_t len, size_t bucket_len)
{
    size_t *B;
    size_t c, d, i, j, g;

    if ((B = calloc(bucket_len, sizeof(size_t))) == NULL)
        return DRPM_ERR_MEMORY;

    for (i = len; i > 0; i--) {
        c = V[i - 1];
        V[i - 1] = B[c];
        B[c] = i;
    }

    for (j = bucket_len - 1, i = len; i > 0; j--) {
        for (d = B[j], g = i; d > 0; i--) {
            c = d - 1;
            d = V[c];
            V[c] = g;
            I[i] = (d == 0 && g == i) ? -1 : (long long)c;
        }
    }

    V[len] = 0;
    I[0] = -1;

    free(B);

    return DRPM_ERR_OK;
}

void suffix_split(long long *I, long long *V, size_t start, size_t len, size_t h)
{
    size_t i, j, k, jj, kk;
    long long x, tmp;
    const size_t end = start + len;

    if (len < 16) {
        for (k = start; k < end; k += j) {
            j = 1;
            x = V[I[k] + h];
            for (i = 1; k + i < end; i++) {
                if (V[I[k+i] + h] < x) {
                    x = V[I[k+i] + h];
                    j = 0;
                }
                if (V[I[k+i] + h] == x) {
                    tmp = I[k+j];
                    I[k+j] = I[k+i];
                    I[k+i] = tmp;
                    j++;
                }
            }
            for (i = 0; i < j; i++)
                V[I[k + i]] = k + j - 1;
            if (j == 1)
                I[k] = -1;
        }
        return;
    }

    x = V[I[start + len/2] + h];
    jj = 0;
    kk = 0;
    for (i = start; i < end; i++) {
        if (V[I[i] + h] < x)
            jj++;
        if (V[I[i] + h] == x)
            kk++;
    }
    jj += start;
    kk += jj;

    i = start;
    j = 0;
    k = 0;
    while (i < jj) {
        if (V[I[i] + h] < x) {
            i++;
        } else if (V[I[i] + h] == x) {
            tmp = I[i];
            I[i] = I[jj + j];
            I[jj + j] = tmp;
            j++;
        } else {
            tmp = I[i];
            I[i] = I[kk + k];
            I[kk + k] = tmp;
            k++;
        }
    }

    while (jj + j < kk) {
        if (V[I[jj+j] + h] == x) {
            j++;
        } else {
            tmp = I[jj + j];
            I[jj + j] = I[kk + k];
            I[kk + k] = tmp;
            k++;
        }
    }

    if(jj > start)
        suffix_split(I, V, start, jj - start, h);

    for (i = 0; i < kk - jj; i++)
        V[I[jj + i]] = kk - 1;
    if (jj == kk - 1)
        I[jj] = -1;

    if (end > kk)
        suffix_split(I, V, kk, end - kk, h);
}

size_t suffix_search(const long long *sfxar,
                     const unsigned char *old, size_t old_len,
                     const unsigned char *new, size_t new_len,
                     size_t start, size_t end,
                     size_t *pos_ret)
{
    size_t halfway;
    size_t len_1;
    size_t len_2;

    if (start > end)
        return 0;

    if (start == end) {
        *pos_ret = sfxar[start];
        return match_len(old + sfxar[start], old_len - sfxar[start], new, new_len);
    }

    while (end - start >= 2) {
        halfway = start + (end - start) / 2;
        if (memcmp(old + sfxar[halfway], new,
                   MIN(new_len, old_len - sfxar[halfway])) < 0)
            start = halfway;
        else
            end = halfway;
    }

    len_1 = match_len(old + sfxar[start], old_len - sfxar[start], new, new_len);
    len_2 = match_len(old + sfxar[end],   old_len - sfxar[end],   new, new_len);

    *pos_ret = sfxar[len_1 > len_2 ? start : end];

    return MAX(len_1, len_2);
}
