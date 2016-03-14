/*
    Authors:
        Matej Chalk <mchalk@redhat.com>

    Copyright (C) 2015 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "drpm.h"
#include "drpm_private.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <rpm/rpmlib.h>
#include <openssl/md5.h>

#define BUFFER_SIZE 4096

/* RFC 4880 - Section 9.4. Hash Algorithms */
#define RFC4880_HASH_ALGO_MD5 1
#define RFC4880_HASH_ALGO_SHA256 8

#define RPMSIG_PADDING(offset) PADDING((offset), 8)

#define RPMLEAD_SIZE 96

struct rpm {
    unsigned char lead[RPMLEAD_SIZE];
    Header signature;
    Header header;
    unsigned char *archive;
    size_t archive_size;
    size_t archive_offset;
    size_t archive_comp_size;
};

static void rpm_init(struct rpm *);
static void rpm_free(struct rpm *);
static int rpm_export_header(struct rpm *, unsigned char **, size_t *);
static int rpm_export_signature(struct rpm *, unsigned char **, size_t *);
static void rpm_header_unload_region(struct rpm *, rpmTagVal);
static int rpm_read_archive(struct rpm *, const char *, off_t, bool,
                            unsigned short *, MD5_CTX *, MD5_CTX *);

void rpm_init(struct rpm *rpmst)
{
    memset(rpmst->lead, 0, RPMLEAD_SIZE);
    rpmst->signature = NULL;
    rpmst->header = NULL;
    rpmst->archive = NULL;
    rpmst->archive_size = 0;
    rpmst->archive_offset = 0;
    rpmst->archive_comp_size = 0;
}

void rpm_free(struct rpm *rpmst)
{
    headerFree(rpmst->signature);
    headerFree(rpmst->header);
    free(rpmst->archive);

    rpm_init(rpmst);
}

int rpm_export_header(struct rpm *rpmst, unsigned char **header_ret, size_t *len_ret)
{
    unsigned char *header;
    unsigned header_size;

    *header_ret = NULL;
    *len_ret = 0;

    if ((header = headerExport(rpmst->header, &header_size)) == NULL ||
        (*header_ret = malloc(sizeof(rpm_header_magic) + header_size)) == NULL) {
        free(header);
        return DRPM_ERR_MEMORY;
    }

    memcpy(*header_ret, rpm_header_magic, sizeof(rpm_header_magic));
    memcpy(*header_ret + sizeof(rpm_header_magic), header, header_size);

    *len_ret = sizeof(rpm_header_magic) + header_size;

    free(header);

    return DRPM_ERR_OK;
}

int rpm_export_signature(struct rpm *rpmst, unsigned char **signature_ret, size_t *len_ret)
{
    unsigned char *signature;
    unsigned signature_size;
    size_t len;
    unsigned char padding[7] = {0};
    unsigned short padding_bytes;

    *signature_ret = NULL;
    *len_ret = 0;

    if ((signature = headerExport(rpmst->signature, &signature_size)) == NULL) {
        free(signature);
        return DRPM_ERR_MEMORY;
    }

    len = sizeof(rpm_header_magic) + signature_size;
    padding_bytes = RPMSIG_PADDING(len);
    len += padding_bytes;

    if ((*signature_ret = malloc(len)) == NULL) {
        free(signature);
        return DRPM_ERR_MEMORY;
    }

    memcpy(*signature_ret, rpm_header_magic, sizeof(rpm_header_magic));
    memcpy(*signature_ret + sizeof(rpm_header_magic), signature, signature_size);
    memcpy(*signature_ret + sizeof(rpm_header_magic) + signature_size, padding, padding_bytes);

    *len_ret = len;

    free(signature);

    return DRPM_ERR_OK;
}

void rpm_header_unload_region(struct rpm *rpmst, rpmTagVal rpmtag)
{
    Header hdr;
    HeaderIterator hdr_iter;
    rpmtd copy_td;
    rpmtd td = rpmtdNew();

    if (headerGet(rpmst->header, rpmtag, td, HEADERGET_DEFAULT)) {
        headerFree(rpmst->header);
        rpmst->header = headerNew();
        copy_td = rpmtdNew();

        hdr = headerCopyLoad(td->data);
        hdr_iter = headerInitIterator(hdr);

        while (headerNext(hdr_iter, copy_td)) {
            if (copy_td->data)
                headerPut(rpmst->header, copy_td, HEADERPUT_DEFAULT);
            rpmtdFreeData(copy_td);
        }

        headerFreeIterator(hdr_iter);
        headerFree(hdr);
        rpmtdFreeData(td);
        rpmtdFree(copy_td);
    }

    rpmtdFree(td);
}

int rpm_read_archive(struct rpm *rpmst, const char *filename,
                     off_t offset, bool decompress, unsigned short *comp_ret,
                     MD5_CTX *seq_md5, MD5_CTX *full_md5)
{
    struct decompstrm *stream = NULL;
    int filedesc;
    unsigned char *archive_tmp;
    unsigned char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    MD5_CTX *md5;
    uint32_t comp;
    int error = DRPM_ERR_OK;

    if ((filedesc = open(filename, O_RDONLY)) < 0)
        return DRPM_ERR_IO;

    if (lseek(filedesc, offset, SEEK_SET) != offset) {
        error = DRPM_ERR_IO;
        goto cleanup;
    }

    if (decompress) {
        // hack: never updating both MD5s when decompressing
        md5 = (seq_md5 == NULL) ? full_md5 : seq_md5;

        if ((error = decompstrm_init(&stream, filedesc, &comp, md5)) != DRPM_ERR_OK ||
            (error = decompstrm_read_until_eof(stream, &rpmst->archive_size, &rpmst->archive)) != DRPM_ERR_OK ||
            (error = decompstrm_get_comp_size(stream, &rpmst->archive_comp_size)) != DRPM_ERR_OK ||
            (error = decompstrm_destroy(&stream)) != DRPM_ERR_OK)
            goto cleanup;

        if (comp_ret != NULL)
            *comp_ret = comp;
    } else {
        while ((bytes_read = read(filedesc, buffer, BUFFER_SIZE)) > 0) {
            if ((archive_tmp = realloc(rpmst->archive,
                 rpmst->archive_size + bytes_read)) == NULL) {
                error = DRPM_ERR_MEMORY;
                goto cleanup;
            }
            if ((seq_md5 != NULL && MD5_Update(seq_md5, buffer, bytes_read) != 1) ||
                (full_md5 != NULL && MD5_Update(full_md5, buffer, bytes_read) != 1)) {
                error = DRPM_ERR_OTHER;
                goto cleanup;
            }
            rpmst->archive = archive_tmp;
            memcpy(rpmst->archive + rpmst->archive_size, buffer, bytes_read);
            rpmst->archive_size += bytes_read;
        }
        if (bytes_read < 0) {
            error = DRPM_ERR_IO;
            goto cleanup;
        }
        rpmst->archive_comp_size = rpmst->archive_size;
    }

cleanup:
    if (stream != NULL)
        decompstrm_destroy(&stream);

    close(filedesc);

    return error;
}

int rpm_read(struct rpm **rpmst, const char *filename,
             int archive_mode, unsigned short *archive_comp,
             unsigned char seq_md5_digest[MD5_DIGEST_LENGTH],
             unsigned char full_md5_digest[MD5_DIGEST_LENGTH])
{
    FD_t file;
    const unsigned char magic_rpm[4] = {0xED, 0xAB, 0xEE, 0xDB};
    off_t file_pos;
    bool include_archive;
    bool decomp_archive = false;
    MD5_CTX seq_md5;
    MD5_CTX full_md5;
    unsigned char *signature = NULL;
    size_t signature_len;
    unsigned char *header = NULL;
    size_t header_len;
    int error = DRPM_ERR_OK;

    if (rpmst == NULL || filename == NULL)
        return DRPM_ERR_PROG;

    switch (archive_mode) {
    case RPM_ARCHIVE_DONT_READ:
        include_archive = false;
        break;
    case RPM_ARCHIVE_READ_UNCOMP:
        include_archive = true;
        decomp_archive = false;
        break;
    case RPM_ARCHIVE_READ_DECOMP:
        include_archive = true;
        decomp_archive = true;
        break;
    default:
        return DRPM_ERR_PROG;
    }

    if ((*rpmst = malloc(sizeof(struct rpm))) == NULL)
        return DRPM_ERR_MEMORY;

    rpm_init(*rpmst);

    // hack: extra '\0' to prevent rpmlib from compressing (see rpmio.c)
    if ((file = Fopen(filename, "rb\0")) == NULL)
        return DRPM_ERR_IO;

    if (Fread((*rpmst)->lead, 1, RPMLEAD_SIZE, file) != RPMLEAD_SIZE ||
        memcmp((*rpmst)->lead, magic_rpm, 4) != 0 ||
        ((*rpmst)->signature = headerRead(file, HEADER_MAGIC_YES)) == NULL ||
        (file_pos = Ftell(file)) < 0 ||
        Fseek(file, RPMSIG_PADDING(file_pos), SEEK_CUR) < 0 ||
        ((*rpmst)->header = headerRead(file, HEADER_MAGIC_YES)) == NULL) {
        error = Ferror(file) ? DRPM_ERR_IO : DRPM_ERR_FORMAT;
        goto cleanup_fail;
    }

    if (seq_md5_digest != NULL) {
        if ((error = rpm_export_header(*rpmst, &header, &header_len)) != DRPM_ERR_OK)
            goto cleanup_fail;
        if (MD5_Init(&seq_md5) != 1 ||
            MD5_Update(&seq_md5, header, header_len) != 1) {
            error = DRPM_ERR_OTHER;
            goto cleanup_fail;
        }
    }

    if (full_md5_digest != NULL) {
        if ((error = rpm_export_signature(*rpmst, &signature, &signature_len)) != DRPM_ERR_OK ||
            (header == NULL && (error = rpm_export_header(*rpmst, &header, &header_len)) != DRPM_ERR_OK))
            goto cleanup_fail;
        if (MD5_Init(&full_md5) != 1 ||
            MD5_Update(&full_md5, (*rpmst)->lead, RPMLEAD_SIZE) != 1 ||
            MD5_Update(&full_md5, signature, signature_len) != 1 ||
            MD5_Update(&full_md5, header, header_len) != 1) {
            error = DRPM_ERR_OTHER;
            goto cleanup_fail;
        }
    }

    if (include_archive) {
        if ((file_pos = Ftell(file)) < 0) {
            error = DRPM_ERR_IO;
            goto cleanup_fail;
        }
        if ((error = rpm_read_archive(*rpmst, filename, file_pos,
                                      decomp_archive, archive_comp,
                                      (seq_md5_digest != NULL) ? &seq_md5 : NULL,
                                      (full_md5_digest != NULL) ? &full_md5 : NULL)) != DRPM_ERR_OK)
            goto cleanup_fail;
    }

    if ((seq_md5_digest != NULL && MD5_Final(seq_md5_digest, &seq_md5) != 1) ||
        (full_md5_digest != NULL && MD5_Final(full_md5_digest, &full_md5) != 1)) {
        error = DRPM_ERR_OTHER;
        goto cleanup_fail;
    }

    goto cleanup;

cleanup_fail:
    rpm_free(*rpmst);

cleanup:
    free(signature);
    free(header);
    Fclose(file);

    return error;
}

int rpm_destroy(struct rpm **rpmst)
{
    if (rpmst == NULL || *rpmst == NULL)
        return DRPM_ERR_PROG;

    rpm_free(*rpmst);
    free(*rpmst);
    *rpmst = NULL;

    return DRPM_ERR_OK;
}

int rpm_archive_read_chunk(struct rpm *rpmst, void *buffer, size_t count)
{
    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    if (rpmst->archive_offset + count >= rpmst->archive_size)
        return DRPM_ERR_FORMAT;

    if (buffer != NULL)
        memcpy(buffer, rpmst->archive + rpmst->archive_offset, count);

    rpmst->archive_offset += count;

    return DRPM_ERR_OK;
}

int rpm_archive_rewind(struct rpm *rpmst)
{
    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    rpmst->archive_offset = 0;

    return DRPM_ERR_OK;
}

uint32_t rpm_size_full(struct rpm *rpmst)
{
    if (rpmst == NULL)
        return 0;

    unsigned sig_size = headerSizeof(rpmst->signature, HEADER_MAGIC_YES);

    return RPMLEAD_SIZE + sig_size + RPMSIG_PADDING(sig_size) +
           headerSizeof(rpmst->header, HEADER_MAGIC_YES) +
           rpmst->archive_comp_size;
}

uint32_t rpm_size_header(struct rpm *rpmst)
{
    if (rpmst == NULL)
        return 0;

    return headerSizeof(rpmst->header, HEADER_MAGIC_YES);
}

int rpm_fetch_lead_and_signature(struct rpm *rpmst,
                                 unsigned char **lead_sig, uint32_t *len_ret)
{
    unsigned char *signature;
    size_t signature_size;
    int error = DRPM_ERR_OK;

    if (rpmst == NULL || lead_sig == NULL || len_ret == NULL)
        return DRPM_ERR_PROG;

    *lead_sig = NULL;
    *len_ret = 0;

    if ((error = rpm_export_signature(rpmst, &signature, &signature_size)) != DRPM_ERR_OK)
        goto cleanup;

    if ((*lead_sig = malloc(RPMLEAD_SIZE + signature_size)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    memcpy(*lead_sig, rpmst->lead, RPMLEAD_SIZE);
    memcpy(*lead_sig + RPMLEAD_SIZE, signature, signature_size);

    *len_ret = RPMLEAD_SIZE + signature_size;

cleanup:
    free(signature);

    return error;
}

int rpm_fetch_header(struct rpm *rpmst, unsigned char **header_ret, uint32_t *len_ret)
{
    int error;
    size_t header_size;

    if ((error = rpm_export_header(rpmst, header_ret, &header_size)) != DRPM_ERR_OK)
        return error;

    *len_ret = header_size;

    return DRPM_ERR_OK;
}

int rpm_fetch_archive(struct rpm *rpmst, unsigned char **archive_ret, size_t *len)
{
    if (rpmst == NULL || archive_ret == NULL || len == NULL)
        return DRPM_ERR_PROG;

    if ((*archive_ret = malloc(rpmst->archive_size)) == NULL)
        return DRPM_ERR_MEMORY;

    memcpy(*archive_ret, rpmst->archive, rpmst->archive_size);
    *len = rpmst->archive_size;

    return DRPM_ERR_OK;
}

int rpm_write(struct rpm *rpmst, const char *filename, bool include_archive)
{
    FD_t file;
    ssize_t padding_bytes;
    unsigned char padding[7] = {0};
    off_t file_pos;
    int error = DRPM_ERR_OK;

    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    // hack: extra '\0' to prevent rpmlib from compressing (see rpmio.c)
    if ((file = Fopen(filename, "wb\0")) == NULL)
        return DRPM_ERR_IO;

    if (Fwrite(rpmst->lead, 1, RPMLEAD_SIZE, file) != RPMLEAD_SIZE) {
        error = DRPM_ERR_IO;
        goto cleanup;
    }

    if (headerWrite(file, rpmst->signature, HEADER_MAGIC_YES) != 0) {
        error = DRPM_ERR_IO;
        goto cleanup;
    }

    if ((file_pos = Ftell(file)) < 0) {
        error = DRPM_ERR_IO;
        goto cleanup;
    }

    if ((padding_bytes = RPMSIG_PADDING(file_pos)) > 0) {
        if (Fwrite(padding, 1, padding_bytes, file) != padding_bytes) {
            error = DRPM_ERR_IO;
            goto cleanup;
        }
    }

    if (headerWrite(file, rpmst->header, HEADER_MAGIC_YES) != 0) {
        error = DRPM_ERR_IO;
        goto cleanup;
    }

    if (include_archive) {
        if (Fwrite(rpmst->archive, 1, rpmst->archive_size, file)
            != (ssize_t)rpmst->archive_size) {
            error = DRPM_ERR_IO;
            goto cleanup;
        }
    }

cleanup:
    Fclose(file);
    return error;
}

int rpm_get_nevr(struct rpm *rpmst, char **nevr)
{
    if (rpmst == NULL || nevr == NULL)
        return DRPM_ERR_PROG;

    if ((*nevr = headerGetAsString(rpmst->header, RPMTAG_NEVR)) == NULL)
        return DRPM_ERR_MEMORY;

    return DRPM_ERR_OK;
}

int rpm_get_comp(struct rpm *rpmst, uint32_t *comp)
{
    const char *payload_comp;

    if (rpmst == NULL || comp == NULL)
        return DRPM_ERR_PROG;

    if ((payload_comp = headerGetString(rpmst->header, RPMTAG_PAYLOADCOMPRESSOR)) == NULL)
        return DRPM_ERR_FORMAT;

    if (strcmp(payload_comp, "gzip") == 0) {
        *comp = DRPM_COMP_GZIP;
    } else if (strcmp(payload_comp, "bzip2") == 0) {
        *comp = DRPM_COMP_BZIP2;
    } else if (strcmp(payload_comp, "lzip") == 0) {
        *comp = DRPM_COMP_LZIP;
    } else if (strcmp(payload_comp, "lzma") == 0) {
        *comp = DRPM_COMP_LZMA;
    } else if (strcmp(payload_comp, "xz") == 0) {
        *comp = DRPM_COMP_XZ;
    } else {
        return DRPM_ERR_FORMAT;
    }

    return DRPM_ERR_OK;
}

int rpm_get_comp_level(struct rpm *rpmst, unsigned short *level)
{
    const char *payload_flags;

    if (rpmst == NULL || level == NULL)
        return DRPM_ERR_PROG;

    if ((payload_flags = headerGetString(rpmst->header, RPMTAG_PAYLOADFLAGS)) == NULL)
        return DRPM_ERR_FORMAT;

    if (strlen(payload_flags) != 1 ||
        payload_flags[0] < '1' || payload_flags[0] > '9')
        return DRPM_ERR_FORMAT;

    *level = payload_flags[0] - '0';

    return DRPM_ERR_OK;
}

int rpm_get_digest_algo(struct rpm *rpmst, unsigned short *digestalgo)
{
    int error = DRPM_ERR_OK;
    rpmtd digest_algo_array;
    uint32_t *digest_algo;

    if (rpmst == NULL || digestalgo == NULL)
        return DRPM_ERR_PROG;

    digest_algo_array = rpmtdNew();

    if (headerGet(rpmst->header, RPMTAG_FILEDIGESTALGO, digest_algo_array,
                  HEADERGET_EXT | HEADERGET_MINMEM) != 1) {
        *digestalgo = DIGESTALGO_MD5;
    } else {
        if ((digest_algo = rpmtdNextUint32(digest_algo_array)) == NULL) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }

        switch (*digest_algo) {
        case RFC4880_HASH_ALGO_MD5:
            *digestalgo = DIGESTALGO_MD5;
            break;
        case RFC4880_HASH_ALGO_SHA256:
            *digestalgo = DIGESTALGO_SHA256;
            break;
        default:
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
    }

cleanup:
    rpmtdFreeData(digest_algo_array);
    rpmtdFree(digest_algo_array);

    return error;
}

int rpm_get_payload_format(struct rpm *rpmst, unsigned short *payfmt)
{
    const char *payload_format;

    if (rpmst == NULL || payfmt == NULL)
        return DRPM_ERR_PROG;

    if ((payload_format = headerGetString(rpmst->header, RPMTAG_PAYLOADFORMAT)) == NULL)
        return DRPM_ERR_MEMORY;

    if (strcmp(payload_format, "drpm") == 0) {
        *payfmt = RPM_PAYLOAD_FORMAT_DRPM;
    } else if (strcmp(payload_format, "cpio") == 0) {
        *payfmt = RPM_PAYLOAD_FORMAT_CPIO;
    } else if (strcmp(payload_format, "xar") == 0) {
        *payfmt = RPM_PAYLOAD_FORMAT_XAR;
    } else {
        return DRPM_ERR_FORMAT;
    }

    return DRPM_ERR_OK;
}

int rpm_patch_payload_format(struct rpm *rpmst, const char *new_payfmt)
{
    if (rpmst == NULL || new_payfmt == NULL)
        return DRPM_ERR_PROG;

    rpm_header_unload_region(rpmst, RPMTAG_HEADERIMMUTABLE);

    if (headerDel(rpmst->header, RPMTAG_PAYLOADFORMAT) != 0)
        return DRPM_ERR_FORMAT;

    if (headerPutString(rpmst->header, RPMTAG_PAYLOADFORMAT, new_payfmt) != 1)
        return DRPM_ERR_FORMAT;

    rpmst->header = headerReload(rpmst->header, RPMTAG_HEADERIMMUTABLE);

    return DRPM_ERR_OK;
}

int rpm_get_file_info(struct rpm *rpmst, struct file_info **files_ret,
                      size_t *count_ret, bool *colors_ret)
{
    int error = DRPM_ERR_OK;
    const struct file_info file_info_init = {0};
    struct file_info *files;
    size_t count;
    bool colors;
    rpmtd filenames;
    rpmtd fileflags;
    rpmtd filemd5s;
    rpmtd filerdevs;
    rpmtd filesizes;
    rpmtd filemodes;
    rpmtd fileverify;
    rpmtd filelinktos;
    rpmtd filecolors;
    const char *name;
    uint32_t *flags;
    const char *md5;
    uint16_t *rdev;
    uint32_t *size;
    uint16_t *mode;
    uint32_t *verify;
    const char *linkto;
    uint32_t *color = NULL;

    if (rpmst == NULL || files_ret == NULL || count_ret == NULL || colors_ret == NULL)
        return DRPM_ERR_PROG;

    filenames = rpmtdNew();
    fileflags = rpmtdNew();
    filemd5s = rpmtdNew();
    filerdevs = rpmtdNew();
    filesizes = rpmtdNew();
    filemodes = rpmtdNew();
    fileverify = rpmtdNew();
    filelinktos = rpmtdNew();
    filecolors = rpmtdNew();

    if (headerGet(rpmst->header, RPMTAG_FILENAMES, filenames, HEADERGET_EXT) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILEFLAGS, fileflags, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILEMD5S, filemd5s, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILERDEVS, filerdevs, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILESIZES, filesizes, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILEMODES, filemodes, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILEVERIFYFLAGS, fileverify, HEADERGET_MINMEM) != 1 ||
        headerGet(rpmst->header, RPMTAG_FILELINKTOS, filelinktos, HEADERGET_MINMEM) != 1) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    colors = (headerGet(rpmst->header, RPMTAG_FILECOLORS, filecolors, HEADERGET_MINMEM) == 1);

    count = rpmtdCount(filenames);
    if (count != rpmtdCount(fileflags) ||
        count != rpmtdCount(filemd5s) ||
        count != rpmtdCount(filerdevs) ||
        count != rpmtdCount(filesizes) ||
        count != rpmtdCount(filemodes) ||
        count != rpmtdCount(fileverify) ||
        count != rpmtdCount(filelinktos) ||
        (colors && count != rpmtdCount(filecolors))) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((files = malloc(count * sizeof(struct file_info))) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    for (size_t i = 0; i < count; i++)
        files[i] = file_info_init;

    for (size_t i = 0; i < count; i++) {
        if ((name = rpmtdNextString(filenames)) == NULL ||
            (flags = rpmtdNextUint32(fileflags)) == NULL ||
            (md5 = rpmtdNextString(filemd5s)) == NULL ||
            (size = rpmtdNextUint32(filesizes)) == NULL ||
            (verify = rpmtdNextUint32(fileverify)) == NULL ||
            (linkto = rpmtdNextString(filelinktos)) == NULL ||
            (colors && (color = rpmtdNextUint32(filecolors)) == NULL) ||
            rpmtdNext(filerdevs) < 0 ||
            rpmtdNext(filemodes) < 0 ||
            (rdev = rpmtdGetUint16(filerdevs)) == NULL ||
            (mode = rpmtdGetUint16(filemodes)) == NULL) {
            error = DRPM_ERR_FORMAT;
            goto cleanup_files;
        }

        if ((files[i].name = malloc(strlen(name) + 1)) == NULL ||
            (files[i].md5 = malloc(strlen(md5) + 1)) == NULL ||
            (files[i].linkto = malloc(strlen(linkto) + 1)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup_files;
        }

        strcpy(files[i].name, name);
        files[i].flags = *flags;
        strcpy(files[i].md5, md5);
        files[i].rdev = *rdev;
        files[i].size = *size;
        files[i].mode = *mode;
        files[i].verify = *verify;
        strcpy(files[i].linkto, linkto);
        if (colors)
            files[i].color = *color;
    }

    *files_ret = files;
    *count_ret = count;
    *colors_ret = colors;

    goto cleanup;

cleanup_files:
    for (size_t i = 0; i < count; i++) {
        free(files[i].name);
        free(files[i].md5);
        free(files[i].linkto);
    }

    free(files);

cleanup:
    rpmtdFreeData(filenames);
    rpmtdFreeData(fileflags);
    rpmtdFreeData(filemd5s);
    rpmtdFreeData(filerdevs);
    rpmtdFreeData(filesizes);
    rpmtdFreeData(filemodes);
    rpmtdFreeData(fileverify);
    rpmtdFreeData(filelinktos);
    rpmtdFreeData(filecolors);

    rpmtdFree(filenames);
    rpmtdFree(fileflags);
    rpmtdFree(filemd5s);
    rpmtdFree(filerdevs);
    rpmtdFree(filesizes);
    rpmtdFree(filemodes);
    rpmtdFree(fileverify);
    rpmtdFree(filelinktos);
    rpmtdFree(filecolors);

    return error;
}

int rpm_find_payload_format_offset(struct rpm *rpmst, uint32_t *offset)
{
    unsigned char *header;
    size_t header_size;
    uint32_t index_count;
    int error;

    if (rpmst == NULL || offset == NULL)
        return DRPM_ERR_PROG;

    if ((error = rpm_export_header(rpmst, &header, &header_size)) != DRPM_ERR_OK)
        return error;

    error = DRPM_ERR_FORMAT;

    index_count = parse_be32(header + 8);

    for (uint32_t i = 0, off = 16; i < index_count && off+16 <= header_size;
         i++, off += 16) {
        if (parse_be32(header + off) == RPMTAG_PAYLOADFORMAT) {
            *offset = parse_be32(header + off + 8);
            error = DRPM_ERR_OK;
            goto cleanup;
        }
    }

cleanup:
    free(header);

    return error;
}

int rpm_signature_empty(struct rpm *rpmst)
{
    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    headerFree(rpmst->signature);
    rpmst->signature = headerNew();

    return DRPM_ERR_OK;
}

int rpm_signature_set_size(struct rpm *rpmst, uint32_t size)
{
    rpmtd tag_data;

    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    tag_data = rpmtdNew();

    tag_data->tag = RPMSIGTAG_SIZE;
    tag_data->type = RPM_INT32_TYPE;
    tag_data->data = &size;
    tag_data->count = 1;

    headerPut(rpmst->signature, tag_data, HEADERPUT_DEFAULT);

    rpmtdFree(tag_data);

    return DRPM_ERR_OK;
}

int rpm_signature_set_md5(struct rpm *rpmst, unsigned char md5[MD5_DIGEST_LENGTH])
{
    rpmtd tag_data;

    if (rpmst == NULL)
        return DRPM_ERR_PROG;

    tag_data = rpmtdNew();

    tag_data->tag = RPMSIGTAG_MD5;
    tag_data->type = RPM_BIN_TYPE;
    tag_data->data = md5;
    tag_data->count = MD5_DIGEST_LENGTH;

    headerPut(rpmst->signature, tag_data, HEADERPUT_DEFAULT);

    rpmtdFree(tag_data);

    return DRPM_ERR_OK;
}

int rpm_signature_reload(struct rpm *rpmst)
{
    rpmst->signature = headerReload(rpmst->signature, RPMTAG_HEADERSIGNATURES);

    return DRPM_ERR_OK;
}
