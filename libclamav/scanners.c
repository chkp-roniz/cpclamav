/*
 *  Copyright (C) 2013-2021 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *  Copyright (C) 2007-2013 Sourcefire, Inc.
 *
 *  Authors: Tomasz Kojm
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#ifndef _WIN32
#include <sys/time.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <fcntl.h>
#include <dirent.h>
#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif

#define DCONF_ARCH ctx->dconf->archive
#define DCONF_DOC ctx->dconf->doc
#define DCONF_MAIL ctx->dconf->mail
#define DCONF_OTHER ctx->dconf->other

#include <zlib.h>

#include "clamav.h"
#include "others.h"
#include "dconf.h"
#include "scanners.h"
#include "matcher-ac.h"
#include "matcher-bm.h"
#include "matcher.h"
#include "ole2_extract.h"
#include "vba_extract.h"
#include "xlm_extract.h"
#include "msexpand.h"
#include "mbox.h"
#include "libmspack.h"
#include "pe.h"
#include "elf.h"
#include "filetypes.h"
#include "htmlnorm.h"
#include "untar.h"
#include "special.h"
#include "binhex.h"
/* #include "uuencode.h" */
#include "tnef.h"
#include "sis.h"
#include "pdf.h"
#include "str.h"
#include "entconv.h"
#include "rtf.h"
#include "unarj.h"
#include "nsis/nulsft.h"
#include "autoit.h"
#include "textnorm.h"
#include "unzip.h"
#include "dlp.h"
#include "default.h"
#include "cpio.h"
#include "macho.h"
#include "ishield.h"
#include "7z_iface.h"
#include "fmap.h"
#include "cache.h"
#include "events.h"
#include "swf.h"
#include "jpeg.h"
#include "gif.h"
#include "png.h"
#include "iso9660.h"
#include "dmg.h"
#include "xar.h"
#include "hfsplus.h"
#include "xz_iface.h"
#include "mbr.h"
#include "gpt.h"
#include "apm.h"
#include "ooxml.h"
#include "xdp.h"
#include "json_api.h"
#include "msxml.h"
#include "tiff.h"
#include "hwp.h"
#include "msdoc.h"
#include "execs.h"
#include "egg.h"

// libclamunrar_iface
#include "unrar_iface.h"

#ifdef HAVE_BZLIB_H
#include <bzlib.h>
#endif

#include <fcntl.h>
#include <string.h>

cl_error_t cli_magic_scan_dir(const char *dirname, cli_ctx *ctx)
{
    DIR *dd;
    struct dirent *dent;
    STATBUF statbuf;
    char *fname;
    unsigned int viruses_found = 0;

    if ((dd = opendir(dirname)) != NULL) {
        while ((dent = readdir(dd))) {
            if (dent->d_ino) {
                if (strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..")) {
                    /* build the full name */
                    fname = cli_malloc(strlen(dirname) + strlen(dent->d_name) + 2);
                    if (!fname) {
                        closedir(dd);
                        cli_dbgmsg("cli_magic_scan_dir: Unable to allocate memory for filename\n");
                        return CL_EMEM;
                    }

                    sprintf(fname, "%s" PATHSEP "%s", dirname, dent->d_name);

                    /* stat the file */
                    if (LSTAT(fname, &statbuf) != -1) {
                        if (S_ISDIR(statbuf.st_mode) && !S_ISLNK(statbuf.st_mode)) {
                            if (cli_magic_scan_dir(fname, ctx) == CL_VIRUS) {
                                free(fname);

                                if (SCAN_ALLMATCHES) {
                                    viruses_found++;
                                    continue;
                                }

                                closedir(dd);
                                return CL_VIRUS;
                            }
                        } else {
                            if (S_ISREG(statbuf.st_mode)) {
                                if (cli_magic_scan_file(fname, ctx, dent->d_name) == CL_VIRUS) {
                                    free(fname);

                                    if (SCAN_ALLMATCHES) {
                                        viruses_found++;
                                        continue;
                                    }

                                    closedir(dd);
                                    return CL_VIRUS;
                                }
                            }
                        }
                    }
                    free(fname);
                }
            }
        }
    } else {
        cli_dbgmsg("cli_magic_scan_dir: Can't open directory %s.\n", dirname);
        return CL_EOPEN;
    }

    closedir(dd);
    if (SCAN_ALLMATCHES && viruses_found)
        return CL_VIRUS;
    return CL_CLEAN;
}

/**
 * @brief  Scan the metadata using cli_matchmeta()
 *
 * @param metadata  unrar metadata structure
 * @param ctx       scanning context structure
 * @param files
 * @return cl_error_t  Returns CL_CLEAN if nothing found, CL_VIRUS if something found, CL_EUNPACK if encrypted.
 */
static cl_error_t cli_unrar_scanmetadata(unrar_metadata_t *metadata, cli_ctx *ctx, unsigned int files)
{
    cl_error_t status = CL_CLEAN;

    cli_dbgmsg("RAR: %s, crc32: 0x%x, encrypted: %u, compressed: %u, normal: %u, method: %u, ratio: %u\n",
               metadata->filename, metadata->crc, metadata->encrypted, (unsigned int)metadata->pack_size,
               (unsigned int)metadata->unpack_size, metadata->method,
               metadata->pack_size ? (unsigned int)(metadata->unpack_size / metadata->pack_size) : 0);

    if (CL_VIRUS == cli_matchmeta(ctx, metadata->filename, metadata->pack_size, metadata->unpack_size, metadata->encrypted, files, metadata->crc, NULL)) {
        status = CL_VIRUS;
    } else if (SCAN_HEURISTIC_ENCRYPTED_ARCHIVE && metadata->encrypted) {
        cli_dbgmsg("RAR: Encrypted files found in archive.\n");
        status = CL_EUNPACK;
    }

    return status;
}

static cl_error_t cli_scanrar(const char *filepath, int desc, cli_ctx *ctx)
{
    cl_error_t status          = CL_EPARSE;
    cl_unrar_error_t unrar_ret = UNRAR_ERR;

    unsigned int file_count    = 0;
    unsigned int viruses_found = 0;

    uint32_t nEncryptedFilesFound = 0;
    uint32_t nTooLargeFilesFound  = 0;

    void *hArchive = NULL;

    char *comment         = NULL;
    uint32_t comment_size = 0;

    unrar_metadata_t metadata;
    char *filename_base    = NULL;
    char *extract_fullpath = NULL;
    char *comment_fullpath = NULL;

    UNUSEDPARAM(desc);

    if (filepath == NULL || ctx == NULL) {
        cli_dbgmsg("RAR: Invalid arguments!\n");
        return CL_EARG;
    }

    cli_dbgmsg("in scanrar()\n");

    /* Zero out the metadata struct before we read the header */
    memset(&metadata, 0, sizeof(unrar_metadata_t));

    /*
     * Open the archive.
     */
    if (UNRAR_OK != (unrar_ret = cli_unrar_open(filepath, &hArchive, &comment, &comment_size, cli_debug_flag))) {
        if (unrar_ret == UNRAR_ENCRYPTED) {
            cli_dbgmsg("RAR: Encrypted main header\n");
            status = CL_EUNPACK;
            goto done;
        }
        if (unrar_ret == UNRAR_EMEM) {
            status = CL_EMEM;
            goto done;
        } else if (unrar_ret == UNRAR_EOPEN) {
            status = CL_EOPEN;
            goto done;
        } else {
            status = CL_EFORMAT;
            goto done;
        }
    }

    /* If the archive header had a comment, write it to the comment dir. */
    if ((comment != NULL) && (comment_size > 0)) {

        if (ctx->engine->keeptmp) {
            int comment_fd = -1;
            if (!(comment_fullpath = cli_gentemp_with_prefix(ctx->sub_tmpdir, "comments"))) {
                status = CL_EMEM;
                goto done;
            }

            comment_fd = open(comment_fullpath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
            if (comment_fd < 0) {
                cli_dbgmsg("RAR: ERROR: Failed to open output file\n");
            } else {
                cli_dbgmsg("RAR: Writing the archive comment to temp file: %s\n", comment_fullpath);
                if (0 == write(comment_fd, comment, comment_size)) {
                    cli_dbgmsg("RAR: ERROR: Failed to write to output file\n");
                }
                close(comment_fd);
            }
        }

        /* Scan the comment */
        status = cli_magic_scan_buff(comment, comment_size, ctx, NULL);

        if ((status == CL_VIRUS) && SCAN_ALLMATCHES) {
            status = CL_CLEAN;
            viruses_found++;
        }
        if ((status == CL_VIRUS) || (status == CL_BREAK)) {
            goto done;
        }
    }

    /*
     * Read & scan each file header.
     * Extract & scan each file.
     *
     * Skip files if they will exceed max filesize or max scansize.
     * Count the number of encrypted file headers and encrypted files.
     *  - Alert if there are encrypted files,
     *      if the Heuristic for encrypted archives is enabled,
     *      and if we have not detected a signature match.
     */
    do {
        status = CL_CLEAN;

        /* Zero out the metadata struct before we read the header */
        memset(&metadata, 0, sizeof(unrar_metadata_t));

        /*
         * Get the header information for the next file in the archive.
         */
        unrar_ret = cli_unrar_peek_file_header(hArchive, &metadata);
        if (unrar_ret != UNRAR_OK) {
            if (unrar_ret == UNRAR_ENCRYPTED) {
                /* Found an encrypted file header, must skip. */
                cli_dbgmsg("RAR: Encrypted file header, unable to reading file metadata and file contents. Skipping file...\n");
                nEncryptedFilesFound += 1;

                if (UNRAR_OK != cli_unrar_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("RAR: Failed to skip file. RAR archive extraction has failed.\n");
                    break;
                }
            } else if (unrar_ret == UNRAR_BREAK) {
                /* No more files. Break extraction loop. */
                cli_dbgmsg("RAR: No more files in archive.\n");
                break;
            } else {
                /* Memory error or some other error reading the header info. */
                cli_dbgmsg("RAR: Error (%u) reading file header!\n", unrar_ret);
                break;
            }
        } else {
            file_count += 1;

            /*
            * Scan the metadata for the file in question since the content was clean, or we're running in all-match.
            */
            status = cli_unrar_scanmetadata(&metadata, ctx, file_count);
            if ((status == CL_VIRUS) && SCAN_ALLMATCHES) {
                status = CL_CLEAN;
                viruses_found++;
            }
            if ((status == CL_VIRUS) || (status == CL_BREAK)) {
                break;
            }

            /* Check if we've already exceeded the scan limit */
            if (cli_checklimits("RAR", ctx, 0, 0, 0))
                break;

            if (metadata.is_dir) {
                /* Entry is a directory. Skip. */
                cli_dbgmsg("RAR: Found directory. Skipping to next file.\n");

                if (UNRAR_OK != cli_unrar_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("RAR: Failed to skip directory. RAR archive extraction has failed.\n");
                    break;
                }
            } else if (cli_checklimits("RAR", ctx, metadata.unpack_size, 0, 0)) {
                /* File size exceeds maxfilesize, must skip extraction.
                * Although we may be able to scan the metadata */
                nTooLargeFilesFound += 1;

                cli_dbgmsg("RAR: Next file is too large (%" PRIu64 " bytes); it would exceed max scansize.  Skipping to next file.\n", metadata.unpack_size);

                if (UNRAR_OK != cli_unrar_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("RAR: Failed to skip file. RAR archive extraction has failed.\n");
                    break;
                }
            } else if (metadata.encrypted != 0) {
                /* Found an encrypted file, must skip. */
                cli_dbgmsg("RAR: Encrypted file, unable to extract file contents. Skipping file...\n");
                nEncryptedFilesFound += 1;

                if (UNRAR_OK != cli_unrar_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("RAR: Failed to skip file. RAR archive extraction has failed.\n");
                    break;
                }
            } else {
                /*
                 * Extract the file...
                 */
                if (NULL != metadata.filename) {
                    (void)cli_basename(metadata.filename, strlen(metadata.filename), &filename_base);
                }

                if (!(ctx->engine->keeptmp) ||
                    (NULL == filename_base)) {
                    extract_fullpath = cli_gentemp(ctx->sub_tmpdir);
                } else {
                    extract_fullpath = cli_gentemp_with_prefix(ctx->sub_tmpdir, filename_base);
                }
                if (NULL == extract_fullpath) {
                    cli_dbgmsg("RAR: Memory error allocating filename for extracted file.");
                    status = CL_EMEM;
                    break;
                }
                cli_dbgmsg("RAR: Extracting file: %s to %s\n", metadata.filename, extract_fullpath);

                unrar_ret = cli_unrar_extract_file(hArchive, extract_fullpath, NULL);
                if (unrar_ret != UNRAR_OK) {
                    /*
                     * Some other error extracting the file
                     */
                    cli_dbgmsg("RAR: Error extracting file: %s\n", metadata.filename);

                    /* TODO:
                     *   may need to manually skip the file depending on what, specifically, cli_unrar_extract_file() returned.
                     */
                } else {
                    /*
                     * File should be extracted...
                     * ... make sure we have read permissions to the file.
                     */
#ifdef _WIN32
                    if (0 != _access_s(extract_fullpath, R_OK)) {
#else
                    if (0 != access(extract_fullpath, R_OK)) {
#endif
                        cli_dbgmsg("RAR: Don't have read permissions, attempting to change file permissions to make it readable..\n");
#ifdef _WIN32
                        if (0 != _chmod(extract_fullpath, _S_IREAD)) {
#else
                        if (0 != chmod(extract_fullpath, S_IRUSR | S_IRGRP)) {
#endif
                            cli_dbgmsg("RAR: Failed to change permission bits so the extracted file is readable..\n");
                        }
                    }

                    /*
                     * ... scan the extracted file.
                     */
                    cli_dbgmsg("RAR: Extraction complete.  Scanning now...\n");
                    status = cli_magic_scan_file(extract_fullpath, ctx, filename_base);
                    if (status == CL_EOPEN) {
                        cli_dbgmsg("RAR: File not found, Extraction failed!\n");
                        status = CL_CLEAN;
                    } else {
                        /* Delete the tempfile if not --leave-temps */
                        if (!ctx->engine->keeptmp)
                            if (cli_unlink(extract_fullpath))
                                cli_dbgmsg("RAR: Failed to unlink the extracted file: %s\n", extract_fullpath);

                        if (status == CL_VIRUS) {
                            cli_dbgmsg("RAR: infected with %s\n", cli_get_last_virus(ctx));
                            status = CL_VIRUS;
                            viruses_found++;
                        }
                    }
                }

                /* Free up that the filepath */
                if (NULL != extract_fullpath) {
                    free(extract_fullpath);
                    extract_fullpath = NULL;
                }
            }
        }

        if (status == CL_VIRUS) {
            if (SCAN_ALLMATCHES)
                status = CL_SUCCESS;
            else
                break;
        }

        if (ctx->engine->maxscansize && ctx->scansize >= ctx->engine->maxscansize) {
            status = CL_CLEAN;
            break;
        }

        /*
         * Free up any malloced metadata...
         */
        if (metadata.filename != NULL) {
            free(metadata.filename);
            metadata.filename = NULL;
        }
        if (NULL != filename_base) {
            free(filename_base);
            filename_base = NULL;
        }

    } while (status == CL_CLEAN);

    if (status == CL_BREAK)
        status = CL_CLEAN;

done:
    if (NULL != comment) {
        free(comment);
        comment = NULL;
    }

    if (NULL != comment_fullpath) {
        if (!ctx->engine->keeptmp) {
            cli_rmdirs(comment_fullpath);
        }
        free(comment_fullpath);
        comment_fullpath = NULL;
    }

    if (NULL != hArchive) {
        cli_unrar_close(hArchive);
        hArchive = NULL;
    }

    if (NULL != filename_base) {
        free(filename_base);
        filename_base = NULL;
    }

    if (metadata.filename != NULL) {
        free(metadata.filename);
        metadata.filename = NULL;
    }

    if (NULL != extract_fullpath) {
        free(extract_fullpath);
        extract_fullpath = NULL;
    }

    if ((CL_VIRUS != status) && ((CL_EUNPACK == status) || (nEncryptedFilesFound > 0))) {
        /* If user requests enabled the Heuristic for encrypted archives... */
        if (SCAN_HEURISTIC_ENCRYPTED_ARCHIVE) {
            if (CL_VIRUS == cli_append_virus(ctx, "Heuristics.Encrypted.RAR")) {
                status = CL_VIRUS;
            }
        }
        if (status != CL_VIRUS) {
            status = CL_CLEAN;
        }
    }

    cli_dbgmsg("RAR: Exit code: %d\n", status);

    if (SCAN_ALLMATCHES && viruses_found)
        status = CL_VIRUS;

    return status;
}

/**
 * @brief  Scan the metadata using cli_matchmeta()
 *
 * @param metadata  egg metadata structure
 * @param ctx       scanning context structure
 * @param files     number of files
 * @return cl_error_t  Returns CL_CLEAN if nothing found, CL_VIRUS if something found, CL_EUNPACK if encrypted.
 */
static cl_error_t cli_egg_scanmetadata(cl_egg_metadata *metadata, cli_ctx *ctx, unsigned int files)
{
    cl_error_t status = CL_CLEAN;

    cli_dbgmsg("EGG: %s, encrypted: %u, compressed: %u, normal: %u, ratio: %u\n",
               metadata->filename, metadata->encrypted, (unsigned int)metadata->pack_size,
               (unsigned int)metadata->unpack_size,
               metadata->pack_size ? (unsigned int)(metadata->unpack_size / metadata->pack_size) : 0);

    if (CL_VIRUS == cli_matchmeta(ctx, metadata->filename, metadata->pack_size, metadata->unpack_size, metadata->encrypted, files, 0, NULL)) {
        status = CL_VIRUS;
    } else if (SCAN_HEURISTIC_ENCRYPTED_ARCHIVE && metadata->encrypted) {
        cli_dbgmsg("EGG: Encrypted files found in archive.\n");
        status = CL_EUNPACK;
    }

    return status;
}

static cl_error_t cli_scanegg(cli_ctx *ctx, size_t sfx_offset)
{
    cl_error_t status  = CL_EPARSE;
    cl_error_t egg_ret = CL_EPARSE;

    unsigned int file_count    = 0;
    unsigned int viruses_found = 0;

    uint32_t nEncryptedFilesFound = 0;
    uint32_t nTooLargeFilesFound  = 0;

    void *hArchive = NULL;

    char **comments    = NULL;
    uint32_t nComments = 0;

    cl_egg_metadata metadata;
    char *filename_base    = NULL;
    char *extract_fullpath = NULL;
    char *comment_fullpath = NULL;

    if (ctx == NULL) {
        cli_dbgmsg("EGG: Invalid arguments!\n");
        return CL_EARG;
    }

    cli_dbgmsg("in scanegg()\n");

    /* Zero out the metadata struct before we read the header */
    memset(&metadata, 0, sizeof(cl_egg_metadata));

    /*
     * Open the archive.
     */
    if (CL_SUCCESS != (egg_ret = cli_egg_open(*ctx->fmap, sfx_offset, &hArchive, &comments, &nComments))) {
        if (egg_ret == CL_EUNPACK) {
            cli_dbgmsg("EGG: Encrypted main header\n");
            status = CL_EUNPACK;
            goto done;
        }
        if (egg_ret == CL_EMEM) {
            status = CL_EMEM;
            goto done;
        } else {
            status = CL_EFORMAT;
            goto done;
        }
    }

    /* If the archive header had a comment, write it to the comment dir. */
    if (comments != NULL) {
        uint32_t i;
        for (i = 0; i < nComments; i++) {
            /*
            * Drop the comment to a temp file, if requested
            */
            if (ctx->engine->keeptmp) {
                int comment_fd   = -1;
                size_t prefixLen = strlen("comments_") + 5;
                char *prefix     = (char *)malloc(prefixLen + 1);

                snprintf(prefix, prefixLen, "comments_%u", i);
                prefix[prefixLen] = '\0';

                if (!(comment_fullpath = cli_gentemp_with_prefix(ctx->sub_tmpdir, prefix))) {
                    free(prefix);
                    status = CL_EMEM;
                    goto done;
                }
                free(prefix);

                comment_fd = open(comment_fullpath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
                if (comment_fd < 0) {
                    cli_dbgmsg("EGG: ERROR: Failed to open output file\n");
                } else {
                    cli_dbgmsg("EGG: Writing the archive comment to temp file: %s\n", comment_fullpath);
                    if (0 == write(comment_fd, comments[i], nComments)) {
                        cli_dbgmsg("EGG: ERROR: Failed to write to output file\n");
                    }
                    close(comment_fd);
                }
                free(comment_fullpath);
                comment_fullpath = NULL;
            }

            /*
            * Scan the comment.
            */
            status = cli_magic_scan_buff(comments[i], strlen(comments[i]), ctx, NULL);

            if ((status == CL_VIRUS) && SCAN_ALLMATCHES) {
                status = CL_CLEAN;
                viruses_found++;
            }
            if ((status == CL_VIRUS) || (status == CL_BREAK)) {
                goto done;
            }
        }
    }

    /*
     * Read & scan each file header.
     * Extract & scan each file.
     *
     * Skip files if they will exceed max filesize or max scansize.
     * Count the number of encrypted file headers and encrypted files.
     *  - Alert if there are encrypted files,
     *      if the Heuristic for encrypted archives is enabled,
     *      and if we have not detected a signature match.
     */
    do {
        status = CL_CLEAN;

        /* Zero out the metadata struct before we read the header */
        memset(&metadata, 0, sizeof(cl_egg_metadata));

        /*
         * Get the header information for the next file in the archive.
         */
        egg_ret = cli_egg_peek_file_header(hArchive, &metadata);
        if (egg_ret != CL_SUCCESS) {
            if (egg_ret == CL_EUNPACK) {
                /* Found an encrypted file header, must skip. */
                cli_dbgmsg("EGG: Encrypted file header, unable to reading file metadata and file contents. Skipping file...\n");
                nEncryptedFilesFound += 1;

                if (CL_SUCCESS != cli_egg_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("EGG: Failed to skip file. EGG archive extraction has failed.\n");
                    break;
                }
            } else if (egg_ret == CL_BREAK) {
                /* No more files. Break extraction loop. */
                cli_dbgmsg("EGG: No more files in archive.\n");
                break;
            } else {
                /* Memory error or some other error reading the header info. */
                cli_dbgmsg("EGG: Error (%u) reading file header!\n", egg_ret);
                break;
            }
        } else {
            file_count += 1;

            /*
            * Scan the metadata for the file in question since the content was clean, or we're running in all-match.
            */
            status = cli_egg_scanmetadata(&metadata, ctx, file_count);
            if ((status == CL_VIRUS) && SCAN_ALLMATCHES) {
                status = CL_CLEAN;
                viruses_found++;
            }
            if ((status == CL_VIRUS) || (status == CL_BREAK)) {
                break;
            }
            /* Check if we've already exceeded the scan limit */
            if (cli_checklimits("EGG", ctx, 0, 0, 0))
                break;

            if (metadata.is_dir) {
                /* Entry is a directory. Skip. */
                cli_dbgmsg("EGG: Found directory. Skipping to next file.\n");

                if (CL_SUCCESS != cli_egg_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("EGG: Failed to skip directory. EGG archive extraction has failed.\n");
                    break;
                }
            } else if (cli_checklimits("EGG", ctx, metadata.unpack_size, 0, 0)) {
                /* File size exceeds maxfilesize, must skip extraction.
                * Although we may be able to scan the metadata */
                nTooLargeFilesFound += 1;

                cli_dbgmsg("EGG: Next file is too large (%" PRIu64 " bytes); it would exceed max scansize.  Skipping to next file.\n", metadata.unpack_size);

                if (CL_SUCCESS != cli_egg_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("EGG: Failed to skip file. EGG archive extraction has failed.\n");
                    break;
                }
            } else if (metadata.encrypted != 0) {
                /* Found an encrypted file, must skip. */
                cli_dbgmsg("EGG: Encrypted file, unable to extract file contents. Skipping file...\n");
                nEncryptedFilesFound += 1;

                if (CL_SUCCESS != cli_egg_skip_file(hArchive)) {
                    /* Failed to skip!  Break extraction loop. */
                    cli_dbgmsg("EGG: Failed to skip file. EGG archive extraction has failed.\n");
                    break;
                }
            } else {
                /*
                * Extract the file...
                */
                char *extract_filename    = NULL;
                char *extract_buffer      = NULL;
                size_t extract_buffer_len = 0;

                cli_dbgmsg("EGG: Extracting file: %s\n", metadata.filename);

                egg_ret = cli_egg_extract_file(hArchive, (const char **)&extract_filename, (const char **)&extract_buffer, &extract_buffer_len);
                if (egg_ret != CL_SUCCESS) {
                    /*
                     * Some other error extracting the file
                     */
                    cli_dbgmsg("EGG: Error extracting file: %s\n", metadata.filename);
                } else if (!extract_buffer || 0 == extract_buffer_len) {
                    /*
                     * Empty file. Skip.
                     */
                    cli_dbgmsg("EGG: Skipping empty file: %s\n", metadata.filename);

                    if (NULL != extract_filename) {
                        free(extract_filename);
                        extract_filename = NULL;
                    }
                    if (NULL != extract_buffer) {
                        free(extract_buffer);
                        extract_buffer = NULL;
                    }
                } else {
                    /*
                     * Drop to a temp file, if requested.
                     */
                    if (NULL != metadata.filename) {
                        (void)cli_basename(metadata.filename, strlen(metadata.filename), &filename_base);
                    }

                    if (ctx->engine->keeptmp) {
                        int extracted_fd = -1;
                        if (NULL == filename_base) {
                            extract_fullpath = cli_gentemp(ctx->sub_tmpdir);
                        } else {
                            extract_fullpath = cli_gentemp_with_prefix(ctx->sub_tmpdir, filename_base);
                        }
                        if (NULL == extract_fullpath) {
                            cli_dbgmsg("EGG: Memory error allocating filename for extracted file.");
                            status = CL_EMEM;
                            break;
                        }

                        extracted_fd = open(extract_fullpath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
                        if (extracted_fd < 0) {
                            cli_dbgmsg("EGG: ERROR: Failed to open output file\n");
                        } else {
                            cli_dbgmsg("EGG: Writing the extracted file contents to temp file: %s\n", extract_fullpath);
                            if (0 == write(extracted_fd, extract_buffer, extract_buffer_len)) {
                                cli_dbgmsg("EGG: ERROR: Failed to write to output file\n");
                            } else {
                                close(extracted_fd);
                                extracted_fd = -1;
                            }
                        }
                    }

                    /*
                     * Scan the extracted file...
                     */
                    cli_dbgmsg("EGG: Extraction complete.  Scanning now...\n");
                    status = cli_magic_scan_buff(extract_buffer, extract_buffer_len, ctx, filename_base);
                    if (status == CL_VIRUS) {
                        cli_dbgmsg("EGG: infected with %s\n", cli_get_last_virus(ctx));
                        status = CL_VIRUS;
                        viruses_found++;
                    }

                    if (NULL != filename_base) {
                        free(filename_base);
                        filename_base = NULL;
                    }
                    if (NULL != extract_filename) {
                        free(extract_filename);
                        extract_filename = NULL;
                    }
                    if (NULL != extract_buffer) {
                        free(extract_buffer);
                        extract_buffer = NULL;
                    }
                }

                /* Free up that the filepath */
                if (NULL != extract_fullpath) {
                    free(extract_fullpath);
                    extract_fullpath = NULL;
                }
            }
        }

        if (status == CL_VIRUS) {
            if (SCAN_ALLMATCHES)
                status = CL_SUCCESS;
            else
                break;
        }

        if (ctx->engine->maxscansize && ctx->scansize >= ctx->engine->maxscansize) {
            status = CL_CLEAN;
            break;
        }

        /*
         * TODO: Free up any malloced metadata...
         */
        if (metadata.filename != NULL) {
            free(metadata.filename);
            metadata.filename = NULL;
        }

    } while (status == CL_CLEAN);

    if (status == CL_BREAK)
        status = CL_CLEAN;

done:

    if (NULL != comment_fullpath) {
        free(comment_fullpath);
        comment_fullpath = NULL;
    }

    if (NULL != hArchive) {
        cli_egg_close(hArchive);
        hArchive = NULL;
    }

    if (NULL != filename_base) {
        free(filename_base);
        filename_base = NULL;
    }

    if (metadata.filename != NULL) {
        free(metadata.filename);
        metadata.filename = NULL;
    }

    if (NULL != extract_fullpath) {
        free(extract_fullpath);
        extract_fullpath = NULL;
    }

    if ((CL_VIRUS != status) && ((CL_EUNPACK == status) || (nEncryptedFilesFound > 0))) {
        /* If user requests enabled the Heuristic for encrypted archives... */
        if (SCAN_HEURISTIC_ENCRYPTED_ARCHIVE) {
            if (CL_VIRUS == cli_append_virus(ctx, "Heuristics.Encrypted.EGG")) {
                status = CL_VIRUS;
            }
        }
        if (status != CL_VIRUS) {
            status = CL_CLEAN;
        }
    }

    cli_dbgmsg("EGG: Exit code: %d\n", status);

    if (SCAN_ALLMATCHES && viruses_found)
        status = CL_VIRUS;

    return status;
}

static cl_error_t cli_scanarj(cli_ctx *ctx, off_t sfx_offset)
{
    cl_error_t ret = CL_CLEAN;
    cl_error_t rc;
    int file = 0;
    arj_metadata_t metadata;
    char *dir;
    int virus_found = 0;

    cli_dbgmsg("in cli_scanarj()\n");

    memset(&metadata, 0, sizeof(arj_metadata_t));

    /* generate the temporary directory */
    if (!(dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "arj-tmp")))
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("ARJ: Can't create temporary directory %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    ret = cli_unarj_open(*ctx->fmap, dir, &metadata, sfx_offset);
    if (ret != CL_SUCCESS) {
        if (!ctx->engine->keeptmp)
            cli_rmdirs(dir);
        free(dir);
        cli_dbgmsg("ARJ: Error: %s\n", cl_strerror(ret));
        return ret;
    }

    do {

        metadata.filename = NULL;
        ret               = cli_unarj_prepare_file(dir, &metadata);
        if (ret != CL_SUCCESS) {
            cli_dbgmsg("ARJ: cli_unarj_prepare_file Error: %s\n", cl_strerror(ret));
            break;
        }
        file++;
        if (cli_matchmeta(ctx, metadata.filename, metadata.comp_size, metadata.orig_size, metadata.encrypted, file, 0, NULL) == CL_VIRUS) {
            if (!SCAN_ALLMATCHES) {
                cli_rmdirs(dir);
                free(dir);
                return CL_VIRUS;
            }
            virus_found = 1;
            ret         = CL_SUCCESS;
        }

        if ((ret = cli_checklimits("ARJ", ctx, metadata.orig_size, metadata.comp_size, 0)) != CL_CLEAN) {
            ret = CL_SUCCESS;
            if (metadata.filename)
                free(metadata.filename);
            continue;
        }
        ret = cli_unarj_extract_file(dir, &metadata);
        if (ret != CL_SUCCESS) {
            cli_dbgmsg("ARJ: cli_unarj_extract_file Error: %s\n", cl_strerror(ret));
        }
        if (metadata.ofd >= 0) {
            if (lseek(metadata.ofd, 0, SEEK_SET) == -1) {
                cli_dbgmsg("ARJ: call to lseek() failed\n");
            }
            rc = cli_magic_scan_desc(metadata.ofd, NULL, ctx, metadata.filename);
            close(metadata.ofd);
            if (rc == CL_VIRUS) {
                cli_dbgmsg("ARJ: infected with %s\n", cli_get_last_virus(ctx));
                if (!SCAN_ALLMATCHES) {
                    ret = CL_VIRUS;
                    if (metadata.filename) {
                        free(metadata.filename);
                        metadata.filename = NULL;
                    }
                    break;
                }
                virus_found = 1;
                ret         = CL_SUCCESS;
            }
        }
        if (metadata.filename) {
            free(metadata.filename);
            metadata.filename = NULL;
        }

    } while (ret == CL_SUCCESS);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    if (metadata.filename) {
        free(metadata.filename);
    }

    if (virus_found != 0)
        ret = CL_VIRUS;
    cli_dbgmsg("ARJ: Exit code: %d\n", ret);
    if (ret == CL_BREAK)
        ret = CL_CLEAN;

    return ret;
}

static cl_error_t cli_scangzip_with_zib_from_the_80s(cli_ctx *ctx, unsigned char *buff)
{
    int fd;
    cl_error_t ret;
    size_t outsize = 0;
    int bytes;
    fmap_t *map = *ctx->fmap;
    char *tmpname;
    gzFile gz;

    ret = fmap_fd(map);
    if (ret < 0)
        return CL_EDUP;
    fd = dup(ret);
    if (fd < 0)
        return CL_EDUP;

    if (!(gz = gzdopen(fd, "rb"))) {
        close(fd);
        return CL_EOPEN;
    }

    if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &fd)) != CL_SUCCESS) {
        cli_dbgmsg("GZip: Can't generate temporary file.\n");
        gzclose(gz);
        close(fd);
        return ret;
    }

    while ((bytes = gzread(gz, buff, FILEBUFF)) > 0) {
        outsize += bytes;
        if (cli_checklimits("GZip", ctx, outsize, 0, 0) != CL_CLEAN)
            break;
        if (cli_writen(fd, buff, (size_t)bytes) != (size_t)bytes) {
            close(fd);
            gzclose(gz);
            if (cli_unlink(tmpname)) {
                free(tmpname);
                return CL_EUNLINK;
            }
            free(tmpname);
            return CL_EWRITE;
        }
    }

    gzclose(gz);

    if ((ret = cli_magic_scan_desc(fd, tmpname, ctx, NULL)) == CL_VIRUS) {
        cli_dbgmsg("GZip: Infected with %s\n", cli_get_last_virus(ctx));
        close(fd);
        if (!ctx->engine->keeptmp) {
            if (cli_unlink(tmpname)) {
                free(tmpname);
                return CL_EUNLINK;
            }
        }
        free(tmpname);
        return CL_VIRUS;
    }
    close(fd);
    if (!ctx->engine->keeptmp)
        if (cli_unlink(tmpname))
            ret = CL_EUNLINK;
    free(tmpname);
    return ret;
}

static cl_error_t cli_scangzip(cli_ctx *ctx)
{
    int fd;
    cl_error_t ret = CL_CLEAN;
    unsigned char buff[FILEBUFF];
    char *tmpname;
    z_stream z;
    size_t at = 0, outsize = 0;
    fmap_t *map = *ctx->fmap;

    cli_dbgmsg("in cli_scangzip()\n");

    memset(&z, 0, sizeof(z));
    if ((ret = inflateInit2(&z, MAX_WBITS + 16)) != Z_OK) {
        cli_dbgmsg("GZip: InflateInit failed: %d\n", ret);
        return cli_scangzip_with_zib_from_the_80s(ctx, buff);
    }

    if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &fd)) != CL_SUCCESS) {
        cli_dbgmsg("GZip: Can't generate temporary file.\n");
        inflateEnd(&z);
        return ret;
    }

    while (at < map->len) {
        unsigned int bytes = MIN(map->len - at, map->pgsz);
        if (!(z.next_in = (void *)fmap_need_off_once(map, at, bytes))) {
            cli_dbgmsg("GZip: Can't read %u bytes @ %lu.\n", bytes, (long unsigned)at);
            inflateEnd(&z);
            close(fd);
            if (cli_unlink(tmpname)) {
                free(tmpname);
                return CL_EUNLINK;
            }
            free(tmpname);
            return CL_EREAD;
        }
        at += bytes;
        z.avail_in = bytes;
        do {
            int inf;
            z.avail_out = sizeof(buff);
            z.next_out  = buff;
            inf         = inflate(&z, Z_NO_FLUSH);
            if (inf != Z_OK && inf != Z_STREAM_END && inf != Z_BUF_ERROR) {
                if (sizeof(buff) == z.avail_out) {
                    cli_dbgmsg("GZip: Bad stream, nothing in output buffer.\n");
                    at = map->len;
                    break;
                } else {
                    cli_dbgmsg("GZip: Bad stream, data in output buffer.\n");
                    /* no break yet, flush extracted bytes to file */
                }
            }
            if (cli_writen(fd, buff, sizeof(buff) - z.avail_out) == (size_t)-1) {
                inflateEnd(&z);
                close(fd);
                if (cli_unlink(tmpname)) {
                    free(tmpname);
                    return CL_EUNLINK;
                }
                free(tmpname);
                return CL_EWRITE;
            }
            outsize += sizeof(buff) - z.avail_out;
            if (cli_checklimits("GZip", ctx, outsize, 0, 0) != CL_CLEAN) {
                at = map->len;
                break;
            }
            if (inf == Z_STREAM_END) {
                at -= z.avail_in;
                inflateReset(&z);
                break;
            } else if (inf != Z_OK && inf != Z_BUF_ERROR) {
                at = map->len;
                break;
            }
        } while (z.avail_out == 0);
    }

    inflateEnd(&z);

    if ((ret = cli_magic_scan_desc(fd, tmpname, ctx, NULL)) == CL_VIRUS) {
        cli_dbgmsg("GZip: Infected with %s\n", cli_get_last_virus(ctx));
        close(fd);
        if (!ctx->engine->keeptmp) {
            if (cli_unlink(tmpname)) {
                free(tmpname);
                return CL_EUNLINK;
            }
        }
        free(tmpname);
        return CL_VIRUS;
    }
    close(fd);
    if (!ctx->engine->keeptmp)
        if (cli_unlink(tmpname))
            ret = CL_EUNLINK;
    free(tmpname);
    return ret;
}

#ifndef HAVE_BZLIB_H
static cl_error_t cli_scanbzip(cli_ctx *ctx)
{
    cli_warnmsg("cli_scanbzip: bzip2 support not compiled in\n");
    return CL_CLEAN;
}

#else

#ifdef NOBZ2PREFIX
#define BZ2_bzDecompressInit bzDecompressInit
#define BZ2_bzDecompress bzDecompress
#define BZ2_bzDecompressEnd bzDecompressEnd
#endif

static cl_error_t cli_scanbzip(cli_ctx *ctx)
{
    cl_error_t ret = CL_CLEAN;
    int fd, rc;
    uint64_t size = 0;
    char *tmpname;
    bz_stream strm;
    size_t off = 0;
    size_t avail;
    char buf[FILEBUFF];

    memset(&strm, 0, sizeof(strm));
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    rc = BZ2_bzDecompressInit(&strm, 0, 0);
    if (BZ_OK != rc) {
        cli_dbgmsg("Bzip: DecompressInit failed: %d\n", rc);
        return CL_EOPEN;
    }

    if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &fd))) {
        cli_dbgmsg("Bzip: Can't generate temporary file.\n");
        BZ2_bzDecompressEnd(&strm);
        return ret;
    }

    do {
        if (!strm.avail_in) {
            strm.next_in = (void *)fmap_need_off_once_len(*ctx->fmap, off, FILEBUFF, &avail);
            strm.avail_in = avail;
            off += avail;
            if (!strm.avail_in) {
                cli_dbgmsg("Bzip: premature end of compressed stream\n");
                break;
            }
        }

        rc = BZ2_bzDecompress(&strm);
        if (BZ_OK != rc && BZ_STREAM_END != rc) {
            cli_dbgmsg("Bzip: decompress error: %d\n", rc);
            break;
        }

        if (!strm.avail_out || BZ_STREAM_END == rc) {

            size += sizeof(buf) - strm.avail_out;

            if (cli_writen(fd, buf, sizeof(buf) - strm.avail_out) != sizeof(buf) - strm.avail_out) {
                cli_dbgmsg("Bzip: Can't write to file.\n");
                BZ2_bzDecompressEnd(&strm);
                close(fd);
                if (!ctx->engine->keeptmp) {
                    if (cli_unlink(tmpname)) {
                        free(tmpname);
                        return CL_EUNLINK;
                    }
                }
                free(tmpname);
                return CL_EWRITE;
            }

            if (cli_checklimits("Bzip", ctx, size, 0, 0) != CL_CLEAN)
                break;

            strm.next_out = buf;
            strm.avail_out = sizeof(buf);
        }
    } while (BZ_STREAM_END != rc);

    BZ2_bzDecompressEnd(&strm);

    if ((ret = cli_magic_scan_desc(fd, tmpname, ctx, NULL)) == CL_VIRUS) {
        cli_dbgmsg("Bzip: Infected with %s\n", cli_get_last_virus(ctx));
        close(fd);
        if (!ctx->engine->keeptmp) {
            if (cli_unlink(tmpname)) {
                ret = CL_EUNLINK;
                free(tmpname);
                return ret;
            }
        }
        free(tmpname);
        return CL_VIRUS;
    }
    close(fd);
    if (!ctx->engine->keeptmp)
        if (cli_unlink(tmpname))
            ret = CL_EUNLINK;
    free(tmpname);

    return ret;
}
#endif

static cl_error_t cli_scanxz(cli_ctx *ctx)
{
    cl_error_t ret = CL_CLEAN;
    int fd, rc;
    unsigned long int size = 0;
    char *tmpname;
    struct CLI_XZ strm;
    size_t off = 0;
    size_t avail;
    unsigned char *buf;

    buf = cli_malloc(CLI_XZ_OBUF_SIZE);
    if (buf == NULL) {
        cli_errmsg("cli_scanxz: nomemory for decompress buffer.\n");
        return CL_EMEM;
    }
    memset(&strm, 0x00, sizeof(struct CLI_XZ));
    strm.next_out  = buf;
    strm.avail_out = CLI_XZ_OBUF_SIZE;
    rc             = cli_XzInit(&strm);
    if (rc != XZ_RESULT_OK) {
        cli_errmsg("cli_scanxz: DecompressInit failed: %i\n", rc);
        free(buf);
        return CL_EOPEN;
    }

    if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &fd))) {
        cli_errmsg("cli_scanxz: Can't generate temporary file.\n");
        cli_XzShutdown(&strm);
        free(buf);
        return ret;
    }
    cli_dbgmsg("cli_scanxz: decompressing to file %s\n", tmpname);

    do {
        /* set up input buffer */
        if (!strm.avail_in) {
            strm.next_in  = (void *)fmap_need_off_once_len(*ctx->fmap, off, CLI_XZ_IBUF_SIZE, &avail);
            strm.avail_in = avail;
            off += avail;
            if (!strm.avail_in) {
                cli_errmsg("cli_scanxz: premature end of compressed stream\n");
                ret = CL_EFORMAT;
                goto xz_exit;
            }
        }

        /* xz decompress a chunk */
        rc = cli_XzDecode(&strm);
        if (XZ_RESULT_OK != rc && XZ_STREAM_END != rc) {
            if (rc == XZ_DIC_HEURISTIC) {
                ret = cli_append_virus(ctx, "Heuristics.XZ.DicSizeLimit");
                goto xz_exit;
            }
            cli_errmsg("cli_scanxz: decompress error: %d\n", rc);
            ret = CL_EFORMAT;
            goto xz_exit;
        }
        //cli_dbgmsg("cli_scanxz: xz decompressed %li of %li available bytes\n",
        //           avail - strm.avail_in, avail);

        /* write decompress buffer */
        if (!strm.avail_out || rc == XZ_STREAM_END) {
            size_t towrite = CLI_XZ_OBUF_SIZE - strm.avail_out;
            size += towrite;

            //cli_dbgmsg("Writing %li bytes to XZ decompress temp file(%li byte total)\n",
            //           towrite, size);

            if (cli_writen(fd, buf, towrite) != towrite) {
                cli_errmsg("cli_scanxz: Can't write to file.\n");
                ret = CL_EWRITE;
                goto xz_exit;
            }
            if (cli_checklimits("cli_scanxz", ctx, size, 0, 0) != CL_CLEAN) {
                cli_warnmsg("cli_scanxz: decompress file size exceeds limits - "
                            "only scanning %li bytes\n",
                            size);
                break;
            }
            strm.next_out  = buf;
            strm.avail_out = CLI_XZ_OBUF_SIZE;
        }
    } while (XZ_STREAM_END != rc);

    /* scan decompressed file */
    if ((ret = cli_magic_scan_desc(fd, tmpname, ctx, NULL)) == CL_VIRUS) {
        cli_dbgmsg("cli_scanxz: Infected with %s\n", cli_get_last_virus(ctx));
    }

xz_exit:
    cli_XzShutdown(&strm);
    close(fd);
    if (!ctx->engine->keeptmp)
        if (cli_unlink(tmpname) && ret == CL_CLEAN)
            ret = CL_EUNLINK;
    free(tmpname);
    free(buf);
    return ret;
}

static cl_error_t cli_scanszdd(cli_ctx *ctx)
{
    int ofd;
    cl_error_t ret;
    char *tmpname;

    cli_dbgmsg("in cli_scanszdd()\n");

    if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &ofd))) {
        cli_dbgmsg("MSEXPAND: Can't generate temporary file/descriptor\n");
        return ret;
    }

    ret = cli_msexpand(ctx, ofd);

    if (ret != CL_SUCCESS) { /* CL_VIRUS or some error */
        close(ofd);
        if (!ctx->engine->keeptmp)
            if (cli_unlink(tmpname))
                ret = CL_EUNLINK;
        free(tmpname);
        return ret;
    }

    cli_dbgmsg("MSEXPAND: Decompressed into %s\n", tmpname);
    ret = cli_magic_scan_desc(ofd, tmpname, ctx, NULL);
    close(ofd);
    if (!ctx->engine->keeptmp)
        if (cli_unlink(tmpname))
            ret = CL_EUNLINK;
    free(tmpname);

    return ret;
}

static cl_error_t vba_scandata(const unsigned char *data, size_t len, cli_ctx *ctx)
{
    struct cli_matcher *groot = ctx->engine->root[0];
    struct cli_matcher *troot = ctx->engine->root[2];
    struct cli_ac_data gmdata, tmdata;
    struct cli_ac_data *mdata[2];
    cl_error_t ret;
    unsigned int viruses_found = 0;

    if ((ret = cli_ac_initdata(&tmdata, troot->ac_partsigs, troot->ac_lsigs, troot->ac_reloff_num, CLI_DEFAULT_AC_TRACKLEN)))
        return ret;

    if ((ret = cli_ac_initdata(&gmdata, groot->ac_partsigs, groot->ac_lsigs, groot->ac_reloff_num, CLI_DEFAULT_AC_TRACKLEN))) {
        cli_ac_freedata(&tmdata);
        return ret;
    }
    mdata[0] = &tmdata;
    mdata[1] = &gmdata;

    ret = cli_scan_buff(data, len, 0, ctx, CL_TYPE_MSOLE2, mdata);
    if (ret == CL_VIRUS)
        viruses_found++;

    if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES)) {
        fmap_t *map = *ctx->fmap;
        *ctx->fmap  = fmap_open_memory(data, len, NULL);
        if (*ctx->fmap == NULL)
            return CL_EMEM;
        ret = cli_exp_eval(ctx, troot, &tmdata, NULL, NULL);
        if (ret == CL_VIRUS)
            viruses_found++;

        if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES))
            ret = cli_exp_eval(ctx, groot, &gmdata, NULL, NULL);
        funmap(*ctx->fmap);
        *ctx->fmap = map;
    }
    cli_ac_freedata(&tmdata);
    cli_ac_freedata(&gmdata);

    return (ret != CL_CLEAN) ? ret : viruses_found ? CL_VIRUS : CL_CLEAN;
}

#define min(x, y) ((x) < (y) ? (x) : (y))

/**
 * Find a file in a directory tree.
 * \param filename Name of the file to find
 * \param dir Directory path where to find the file
 * \param A pointer to the string to store the result into
 * \param Size of the string to store the result in
 */
cl_error_t find_file(const char *filename, const char *dir, char *result, size_t result_size)
{
    DIR *dd;
    struct dirent *dent;
    char fullname[PATH_MAX];
    cl_error_t ret;
    size_t len;
    STATBUF statbuf;

    if (!result) {
        return CL_ENULLARG;
    }

    if ((dd = opendir(dir)) != NULL) {
        while ((dent = readdir(dd))) {
            if (dent->d_ino) {
                if (strcmp(dent->d_name, ".") != 0 && strcmp(dent->d_name, "..") != 0) {

                    snprintf(fullname, sizeof(fullname), "%s" PATHSEP "%s", dir, dent->d_name);
                    fullname[sizeof(fullname) - 1] = '\0';

                    /* stat the file */
                    if (LSTAT(fullname, &statbuf) != -1) {
                        if (S_ISDIR(statbuf.st_mode) && !S_ISLNK(statbuf.st_mode)) {
                            ret = find_file(filename, fullname, result, result_size);
                            if (ret == CL_SUCCESS) {
                                closedir(dd);
                                return ret;
                            }
                        } else if (S_ISREG(statbuf.st_mode)) {
                            if (strcmp(dent->d_name, filename) == 0) {
                                len = min(strlen(dir) + 1, result_size);
                                memcpy(result, dir, len);
                                result[len - 1] = '\0';
                                closedir(dd);
                                return CL_SUCCESS;
                            }
                        }
                    }
                }
            }
        }
        closedir(dd);
    }

    return CL_EOPEN;
}

/**
 * Scan an OLE directory for a VBA project.
 * Contrary to cli_vba_scandir, this function uses the dir file to locate VBA modules.
 */
static cl_error_t cli_vba_scandir_new(const char *dirname, cli_ctx *ctx, struct uniq *U, int *has_macros)
{
    cl_error_t ret   = CL_SUCCESS;
    uint32_t hashcnt = 0;
    char *hash       = NULL;
    char path[PATH_MAX];
    char filename[PATH_MAX];
    int tempfd        = -1;
    int viruses_found = 0;

    if (CL_SUCCESS != (ret = uniq_get(U, "dir", 3, &hash, &hashcnt))) {
        cli_dbgmsg("cli_vba_scandir_new: uniq_get('dir') failed with ret code (%d)!\n", ret);
        return ret;
    }

    while (hashcnt) {
        //Find the directory containing the extracted dir file. This is complicated
        //because ClamAV doesn't use the file names from the OLE file, but temporary names,
        //and we have neither the complete path of the dir file in the OLE container,
        //nor the mapping of the temporary directory names to their OLE names.
        snprintf(filename, sizeof(filename), "%s_%u", hash, hashcnt);
        filename[sizeof(filename) - 1] = '\0';

        if (CL_SUCCESS == find_file(filename, dirname, path, sizeof(path))) {
            cli_dbgmsg("cli_vba_scandir_new: Found dir file: %s\n", path);
            if ((ret = cli_vba_readdir_new(ctx, path, U, hash, hashcnt, &tempfd, has_macros)) != CL_SUCCESS) {
                //FIXME: Since we only know the stream name of the OLE2 stream, but not its path inside the
                //       OLE2 archive, we don't know if we have the right file. The only thing we can do is
                //       iterate all of them until one succeeds.
                cli_dbgmsg("cli_vba_scandir_new: Failed to read dir from %s, trying others (error: %s (%d))\n", path, cl_strerror(ret), (int)ret);
                ret = CL_SUCCESS;
                hashcnt--;
                continue;
            }

#if HAVE_JSON
            if (*has_macros && SCAN_COLLECT_METADATA && (ctx->wrkproperty != NULL)) {
                cli_jsonbool(ctx->wrkproperty, "HasMacros", 1);
                json_object *macro_languages = cli_jsonarray(ctx->wrkproperty, "MacroLanguages");
                if (macro_languages) {
                    cli_jsonstr(macro_languages, NULL, "VBA");
                } else {
                    cli_dbgmsg("[cli_vba_scandir_new] Failed to add \"VBA\" entry to MacroLanguages JSON array\n");
                }
            }
#endif
            if (SCAN_HEURISTIC_MACROS && *has_macros) {
                ret = cli_append_virus(ctx, "Heuristics.OLE2.ContainsMacros.VBA");
                if (ret == CL_VIRUS) {
                    viruses_found++;
                    if (!SCAN_ALLMATCHES) {
                        goto done;
                    }
                }
            }

            /*
             * Now rewind the extracted vba-project output FD and scan it!
             */
            if (lseek(tempfd, 0, SEEK_SET) != 0) {
                cli_dbgmsg("cli_vba_scandir_new: Failed to seek to beginning of temporary VBA project file\n");
                ret = CL_ESEEK;
                goto done;
            }

            ctx->recursion += 1;
            cli_set_container(ctx, CL_TYPE_MSOLE2, 0); //TODO: set correct container size

            ret = cli_scan_desc(tempfd, ctx, CL_TYPE_SCRIPT, 0, NULL, AC_SCAN_VIR, NULL, NULL);

            close(tempfd);
            tempfd = -1;
            ctx->recursion -= 1;

            if (CL_VIRUS == ret) {
                viruses_found++;
                if (!SCAN_ALLMATCHES) {
                    goto done;
                }
            }
        }

        hashcnt--;
    }

done:
    if (tempfd != -1) {
        close(tempfd);
        tempfd = -1;
    }

    if (viruses_found > 0)
        ret = CL_VIRUS;
    return ret;
}

static cl_error_t cli_vba_scandir(const char *dirname, cli_ctx *ctx, struct uniq *U, int *has_macros)
{
    cl_error_t status = CL_CLEAN;
    cl_error_t ret;
    int i, j;
    size_t data_len;
    vba_project_t *vba_project;
    DIR *dd = NULL;
    struct dirent *dent;
    STATBUF statbuf;
    char *fullname, vbaname[1024];
    unsigned char *data;
    char *hash;
    uint32_t hashcnt           = 0;
    unsigned int viruses_found = 0;

    cli_dbgmsg("VBADir: %s\n", dirname);
    if (CL_SUCCESS != (ret = uniq_get(U, "_vba_project", 12, NULL, &hashcnt))) {
        cli_dbgmsg("VBADir: uniq_get('_vba_project') failed with ret code (%d)!\n", ret);
        status = ret;
        goto done;
    }
    while (hashcnt) {
        if (!(vba_project = (vba_project_t *)cli_vba_readdir(dirname, U, hashcnt))) {
            hashcnt--;
            continue;
        }

        for (i = 0; i < vba_project->count; i++) {
            for (j = 1; (unsigned int)j <= vba_project->colls[i]; j++) {
                int fd = -1;

                snprintf(vbaname, 1024, "%s" PATHSEP "%s_%u", vba_project->dir, vba_project->name[i], j);
                vbaname[sizeof(vbaname) - 1] = '\0';

                fd = open(vbaname, O_RDONLY | O_BINARY);
                if (fd == -1) {
                    continue;
                }
                cli_dbgmsg("VBADir: Decompress VBA project '%s_%u'\n", vba_project->name[i], j);
                data = (unsigned char *)cli_vba_inflate(fd, vba_project->offset[i], &data_len);
                close(fd);
                *has_macros = *has_macros + 1;
                if (!data) {
                } else {
                    /* cli_dbgmsg("Project content:\n%s", data); */
                    if (ctx->scanned)
                        *ctx->scanned += data_len / CL_COUNT_PRECISION;
                    if (ctx->engine->keeptmp) {
                        char *tempfile;
                        int of;

                        if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tempfile, &of)) != CL_SUCCESS) {
                            cli_warnmsg("VBADir: WARNING: VBA project '%s_%u' cannot be dumped to file\n", vba_project->name[i], j);
                            status = ret;
                            goto done;
                        }
                        if (cli_writen(of, data, data_len) != data_len) {
                            cli_warnmsg("VBADir: WARNING: VBA project '%s_%u' failed to write to file\n", vba_project->name[i], j);
                            close(of);
                            free(tempfile);
                            status = CL_EWRITE;
                            goto done;
                        }

                        cli_dbgmsg("VBADir: VBA project '%s_%u' dumped to %s\n", vba_project->name[i], j, tempfile);
                        free(tempfile);
                    }

                    if (vba_scandata(data, data_len, ctx) == CL_VIRUS) {
                        viruses_found++;
                        if (!SCAN_ALLMATCHES) {
                            free(data);
                            status = CL_VIRUS;
                            break;
                        }
                    }
                    free(data);
                }
            }

            if (status == CL_VIRUS)
                break;
        }

        cli_free_vba_project(vba_project);
        vba_project = NULL;

        if (status == CL_VIRUS)
            break;

        hashcnt--;
    }

    if (status == CL_CLEAN || (status == CL_VIRUS && SCAN_ALLMATCHES)) {
        if (CL_SUCCESS != (ret = uniq_get(U, "powerpoint document", 19, &hash, &hashcnt))) {
            cli_dbgmsg("VBADir: uniq_get('powerpoint document') failed with ret code (%d)!\n", ret);
            status = ret;
            goto done;
        }
        while (hashcnt) {
            int fd = -1;

            snprintf(vbaname, 1024, "%s" PATHSEP "%s_%u", dirname, hash, hashcnt);
            vbaname[sizeof(vbaname) - 1] = '\0';

            fd = open(vbaname, O_RDONLY | O_BINARY);
            if (fd == -1) {
                hashcnt--;
                continue;
            }
            if ((fullname = cli_ppt_vba_read(fd, ctx))) {
                ret = cli_magic_scan_dir(fullname, ctx);

                if (!ctx->engine->keeptmp)
                    cli_rmdirs(fullname);
                free(fullname);

                if (ret == CL_VIRUS) {
                    status = CL_VIRUS;
                    viruses_found++;
                    if (!SCAN_ALLMATCHES) {
                        close(fd);
                        break;
                    }
                }
            }
            close(fd);
            hashcnt--;
        }
    }

    if (status == CL_CLEAN || (status == CL_VIRUS && SCAN_ALLMATCHES)) {
        if (CL_SUCCESS != (ret = uniq_get(U, "worddocument", 12, &hash, &hashcnt))) {
            cli_dbgmsg("VBADir: uniq_get('worddocument') failed with ret code (%d)!\n", ret);
            status = ret;
            goto done;
        }
        while (hashcnt) {
            int fd = -1;

            snprintf(vbaname, sizeof(vbaname), "%s" PATHSEP "%s_%u", dirname, hash, hashcnt);
            vbaname[sizeof(vbaname) - 1] = '\0';

            fd = open(vbaname, O_RDONLY | O_BINARY);
            if (fd == -1) {
                hashcnt--;
                continue;
            }

            if (!(vba_project = (vba_project_t *)cli_wm_readdir(fd))) {
                close(fd);
                hashcnt--;
                continue;
            }

            for (i = 0; i < vba_project->count; i++) {
                cli_dbgmsg("VBADir: Decompress WM project macro:%d key:%d length:%d\n", i, vba_project->key[i], vba_project->length[i]);
                data = (unsigned char *)cli_wm_decrypt_macro(fd, vba_project->offset[i], vba_project->length[i], vba_project->key[i]);
                if (!data) {
                    cli_dbgmsg("VBADir: WARNING: WM project '%s' macro %d decrypted to NULL\n", vba_project->name[i], i);
                } else {
                    cli_dbgmsg("Project content:\n%s", data);
                    if (ctx->scanned)
                        *ctx->scanned += vba_project->length[i] / CL_COUNT_PRECISION;
                    if (vba_scandata(data, vba_project->length[i], ctx) == CL_VIRUS) {
                        viruses_found++;
                        if (!SCAN_ALLMATCHES) {
                            free(data);
                            status = CL_VIRUS;
                            break;
                        }
                    }
                    free(data);
                }
            }

            close(fd);
            cli_free_vba_project(vba_project);
            vba_project = NULL;

            if (status == CL_VIRUS && !SCAN_ALLMATCHES) {
                break;
            }
            hashcnt--;
        }
    }

#if HAVE_JSON
    /* JSON Output Summary Information */
    if (SCAN_COLLECT_METADATA && (ctx->wrkproperty != NULL)) {
        if (CL_SUCCESS != (ret = uniq_get(U, "_5_summaryinformation", 21, &hash, &hashcnt))) {
            cli_dbgmsg("VBADir: uniq_get('_5_summaryinformation') failed with ret code (%d)!\n", ret);
            status = ret;
            goto done;
        }
        while (hashcnt) {
            int fd = -1;

            snprintf(vbaname, sizeof(vbaname), "%s" PATHSEP "%s_%u", dirname, hash, hashcnt);
            vbaname[sizeof(vbaname) - 1] = '\0';

            fd = open(vbaname, O_RDONLY | O_BINARY);
            if (fd >= 0) {
                cli_dbgmsg("VBADir: detected a '_5_summaryinformation' stream\n");
                /* JSONOLE2 - what to do if something breaks? */
                cli_ole2_summary_json(ctx, fd, 0);
                close(fd);
            }
            hashcnt--;
        }

        if (CL_SUCCESS != (ret = uniq_get(U, "_5_documentsummaryinformation", 29, &hash, &hashcnt))) {
            cli_dbgmsg("VBADir: uniq_get('_5_documentsummaryinformation') failed with ret code (%d)!\n", ret);
            status = ret;
            goto done;
        }
        while (hashcnt) {
            int fd = -1;

            snprintf(vbaname, sizeof(vbaname), "%s" PATHSEP "%s_%u", dirname, hash, hashcnt);
            vbaname[sizeof(vbaname) - 1] = '\0';

            fd = open(vbaname, O_RDONLY | O_BINARY);
            if (fd >= 0) {
                cli_dbgmsg("VBADir: detected a '_5_documentsummaryinformation' stream\n");
                /* JSONOLE2 - what to do if something breaks? */
                cli_ole2_summary_json(ctx, fd, 1);
                close(fd);
            }
            hashcnt--;
        }
    }
#endif

    if (status != CL_CLEAN && !(status == CL_VIRUS && SCAN_ALLMATCHES)) {
        goto done;
    }

    /* Check directory for embedded OLE objects */
    if (CL_SUCCESS != (ret = uniq_get(U, "_1_ole10native", 14, &hash, &hashcnt))) {
        cli_dbgmsg("VBADir: uniq_get('_1_ole10native') failed with ret code (%d)!\n", ret);
        status = ret;
        goto done;
    }
    while (hashcnt) {
        int fd = -1;

        snprintf(vbaname, sizeof(vbaname), "%s" PATHSEP "%s_%u", dirname, hash, hashcnt);
        vbaname[sizeof(vbaname) - 1] = '\0';

        fd = open(vbaname, O_RDONLY | O_BINARY);
        if (fd >= 0) {
            ret = cli_scan_ole10(fd, ctx);
            close(fd);
            if (CL_VIRUS == ret) {
                viruses_found++;
                if (!SCAN_ALLMATCHES) {
                    status = ret;
                    goto done;
                }
            }
        }
        hashcnt--;
    }

    /* ACAB: since we now hash filenames and handle collisions we
     * could avoid recursion by removing the block below and by
     * flattening the paths in ole2_walk_property_tree (case 1) */

    if ((dd = opendir(dirname)) != NULL) {
        while ((dent = readdir(dd))) {
            if (dent->d_ino) {
                if (strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..")) {
                    /* build the full name */
                    fullname = cli_malloc(strlen(dirname) + strlen(dent->d_name) + 2);
                    if (!fullname) {
                        cli_dbgmsg("cli_vba_scandir: Unable to allocate memory for fullname\n");
                        status = CL_EMEM;
                        break;
                    }
                    sprintf(fullname, "%s" PATHSEP "%s", dirname, dent->d_name);

                    /* stat the file */
                    if (LSTAT(fullname, &statbuf) != -1) {
                        if (S_ISDIR(statbuf.st_mode) && !S_ISLNK(statbuf.st_mode))
                            if (cli_vba_scandir(fullname, ctx, U, has_macros) == CL_VIRUS) {
                                viruses_found++;
                                if (!SCAN_ALLMATCHES) {
                                    status = CL_VIRUS;
                                    free(fullname);
                                    break;
                                }
                            }
                    }
                    free(fullname);
                }
            }
        }
    } else {
        cli_dbgmsg("VBADir: Can't open directory %s.\n", dirname);
        status = CL_EOPEN;
        goto done;
    }

done:
    if (NULL != dd) {
        closedir(dd);
    }

#if HAVE_JSON
    if (*has_macros && SCAN_COLLECT_METADATA && (ctx->wrkproperty != NULL)) {
        cli_jsonbool(ctx->wrkproperty, "HasMacros", 1);
        json_object *macro_languages = cli_jsonarray(ctx->wrkproperty, "MacroLanguages");
        if (macro_languages) {
            cli_jsonstr(macro_languages, NULL, "VBA");
        } else {
            cli_dbgmsg("[cli_scan_vbadir] Failed to add \"VBA\" entry to MacroLanguages JSON array\n");
        }
    }
#endif
    if (SCAN_HEURISTIC_MACROS && *has_macros) {
        ret = cli_append_virus(ctx, "Heuristics.OLE2.ContainsMacros.VBA");
        if (ret == CL_VIRUS)
            viruses_found++;
    }

    if (viruses_found > 0) {
        status = CL_VIRUS;
    }
    return status;
}

static cl_error_t cli_xlm_scandir(const char *dirname, cli_ctx *ctx, struct uniq *U)
{
    cl_error_t ret             = CL_CLEAN;
    char *hash                 = NULL;
    uint32_t hashcnt           = 0;
    unsigned int viruses_found = 0;
    char STR_WORKBOOK[]        = "workbook";
    char STR_BOOK[]            = "book";

    cli_dbgmsg("XLMDir: %s\n", dirname);

    if (CL_SUCCESS != (ret = uniq_get(U, STR_WORKBOOK, sizeof(STR_WORKBOOK) - 1, &hash, &hashcnt))) {
        if (CL_SUCCESS != (ret = uniq_get(U, STR_BOOK, sizeof(STR_BOOK) - 1, &hash, &hashcnt))) {
            cli_dbgmsg("XLMDir: uniq_get('%s') failed with ret code (%d)!\n", STR_BOOK, ret);
            return ret;
        }
    }

    for (; hashcnt > 0; hashcnt--) {
        if ((ret = cli_xlm_extract_macros(dirname, ctx, U, hash, hashcnt)) != CL_SUCCESS) {
            switch (ret) {
                case CL_VIRUS:
                case CL_EMEM:
                    return ret;
                default:
                    cli_dbgmsg("XLMDir: An error occured when parsing XLM BIFF temp file, skipping to next file.\n");
            }
        }
    }

    if (SCAN_HEURISTIC_MACROS) {
        ret = cli_append_virus(ctx, "Heuristics.OLE2.ContainsMacros.XLM");
        if (ret == CL_VIRUS)
            viruses_found++;
    }
    if (SCAN_ALLMATCHES && viruses_found)
        return CL_VIRUS;
    return ret;
}

static cl_error_t cli_scanhtml(cli_ctx *ctx)
{
    char *tempname, fullname[1024];
    cl_error_t ret = CL_CLEAN;
    int fd;
    fmap_t *map                = *ctx->fmap;
    unsigned int viruses_found = 0;
    uint64_t curr_len          = map->len;

    cli_dbgmsg("in cli_scanhtml()\n");

    /* CL_ENGINE_MAX_HTMLNORMALIZE */
    if (curr_len > ctx->engine->maxhtmlnormalize) {
        cli_dbgmsg("cli_scanhtml: exiting (file larger than MaxHTMLNormalize)\n");
        return CL_CLEAN;
    }

    if (!(tempname = cli_gentemp_with_prefix(ctx->sub_tmpdir, "html-tmp")))
        return CL_EMEM;

    if (mkdir(tempname, 0700)) {
        cli_errmsg("cli_scanhtml: Can't create temporary directory %s\n", tempname);
        free(tempname);
        return CL_ETMPDIR;
    }

    cli_dbgmsg("cli_scanhtml: using tempdir %s\n", tempname);

    html_normalise_map(map, tempname, NULL, ctx->dconf);
    snprintf(fullname, 1024, "%s" PATHSEP "nocomment.html", tempname);
    fd = open(fullname, O_RDONLY | O_BINARY);
    if (fd >= 0) {
        if ((ret = cli_scan_desc(fd, ctx, CL_TYPE_HTML, 0, NULL, AC_SCAN_VIR, NULL, NULL)) == CL_VIRUS)
            viruses_found++;
        close(fd);
    }

    if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES)) {
        /* CL_ENGINE_MAX_HTMLNOTAGS */
        curr_len = map->len;
        if (curr_len > ctx->engine->maxhtmlnotags) {
            /* we're not interested in scanning large files in notags form */
            /* TODO: don't even create notags if file is over limit */
            cli_dbgmsg("cli_scanhtml: skipping notags (normalized size over MaxHTMLNoTags)\n");
        } else {
            snprintf(fullname, 1024, "%s" PATHSEP "notags.html", tempname);
            fd = open(fullname, O_RDONLY | O_BINARY);
            if (fd >= 0) {
                if ((ret = cli_scan_desc(fd, ctx, CL_TYPE_HTML, 0, NULL, AC_SCAN_VIR, NULL, NULL)) == CL_VIRUS)
                    viruses_found++;
                close(fd);
            }
        }
    }

    if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES)) {
        snprintf(fullname, 1024, "%s" PATHSEP "javascript", tempname);
        fd = open(fullname, O_RDONLY | O_BINARY);
        if (fd >= 0) {
            if ((ret = cli_scan_desc(fd, ctx, CL_TYPE_HTML, 0, NULL, AC_SCAN_VIR, NULL, NULL)) == CL_VIRUS)
                viruses_found++;
            if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES)) {
                if ((ret = cli_scan_desc(fd, ctx, CL_TYPE_TEXT_ASCII, 0, NULL, AC_SCAN_VIR, NULL, NULL)) == CL_VIRUS)
                    viruses_found++;
            }
            close(fd);
        }
    }

    if (ret == CL_CLEAN || (ret == CL_VIRUS && SCAN_ALLMATCHES)) {
        snprintf(fullname, 1024, "%s" PATHSEP "rfc2397", tempname);
        ret = cli_magic_scan_dir(fullname, ctx);
        if (CL_EOPEN == ret) {
            /* If the directory doesn't exist, that's fine */
            ret = CL_CLEAN;
        }
    }

    if (!ctx->engine->keeptmp)
        cli_rmdirs(tempname);

    free(tempname);
    if (SCAN_ALLMATCHES && viruses_found)
        return CL_VIRUS;
    return ret;
}

static cl_error_t cli_scanscript(cli_ctx *ctx)
{
    const unsigned char *buff;
    unsigned char *normalized = NULL;
    struct text_norm_state state;
    char *tmpname = NULL;
    int ofd       = -1;
    cl_error_t ret;
    struct cli_matcher *troot;
    uint32_t maxpatlen, offset = 0;
    struct cli_matcher *groot;
    struct cli_ac_data gmdata, tmdata;
    int gmdata_initialized = 0;
    int tmdata_initialized = 0;
    struct cli_ac_data *mdata[2];
    fmap_t *map;
    size_t at                  = 0;
    unsigned int viruses_found = 0;
    uint64_t curr_len;
    struct cli_target_info info;

    if (!ctx || !ctx->engine->root)
        return CL_ENULLARG;

    map       = *ctx->fmap;
    curr_len  = map->len;
    groot     = ctx->engine->root[0];
    troot     = ctx->engine->root[7];
    maxpatlen = troot ? troot->maxpatlen : 0;

    // Initialize info so it's safe to pass to destroy later
    cli_targetinfo_init(&info);

    cli_dbgmsg("in cli_scanscript()\n");

    /* CL_ENGINE_MAX_SCRIPTNORMALIZE */
    if (curr_len > ctx->engine->maxscriptnormalize) {
        cli_dbgmsg("cli_scanscript: exiting (file larger than MaxScriptSize)\n");
        ret = CL_CLEAN;
        goto done;
    }

    if (!(normalized = cli_malloc(SCANBUFF + maxpatlen))) {
        cli_dbgmsg("cli_scanscript: Unable to malloc %u bytes\n", SCANBUFF);
        ret = CL_EMEM;
        goto done;
    }
    text_normalize_init(&state, normalized, SCANBUFF + maxpatlen);

    if ((ret = cli_ac_initdata(&tmdata, troot ? troot->ac_partsigs : 0, troot ? troot->ac_lsigs : 0, troot ? troot->ac_reloff_num : 0, CLI_DEFAULT_AC_TRACKLEN))) {
        goto done;
    }
    tmdata_initialized = 1;

    if ((ret = cli_ac_initdata(&gmdata, groot->ac_partsigs, groot->ac_lsigs, groot->ac_reloff_num, CLI_DEFAULT_AC_TRACKLEN))) {
        goto done;
    }
    gmdata_initialized = 1;

    /* dump to disk only if explicitly asked to
     * or if necessary to check relative offsets,
     * otherwise we can process just in-memory */
    if (ctx->engine->keeptmp || (troot && (troot->ac_reloff_num > 0 || troot->linked_bcs))) {
        if ((ret = cli_gentempfd(ctx->sub_tmpdir, &tmpname, &ofd))) {
            cli_dbgmsg("cli_scanscript: Can't generate temporary file/descriptor\n");
            goto done;
        }
        if (ctx->engine->keeptmp)
            cli_dbgmsg("cli_scanscript: saving normalized file to %s\n", tmpname);
    }

    mdata[0] = &tmdata;
    mdata[1] = &gmdata;

    /* If there's a relative offset in troot or triggered bytecodes, normalize to file.*/
    if (troot && (troot->ac_reloff_num > 0 || troot->linked_bcs)) {
        size_t map_off = 0;
        while (map_off < map->len) {
            size_t written;
            if (!(written = text_normalize_map(&state, map, map_off)))
                break;
            map_off += written;

            if (write(ofd, state.out, state.out_pos) == -1) {
                cli_errmsg("cli_scanscript: can't write to file %s\n", tmpname);
                ret = CL_EWRITE;
                goto done;
            }
            text_normalize_reset(&state);
        }

        /* Temporarily store the normalized file map in the context. */
        *ctx->fmap = fmap(ofd, 0, 0, NULL);
        if (!(*ctx->fmap)) {
            cli_dbgmsg("cli_scanscript: could not map file %s\n", tmpname);
        } else {

            /* scan map */
            ret = cli_scan_fmap(ctx, CL_TYPE_TEXT_ASCII, 0, NULL, AC_SCAN_VIR, NULL, NULL);
            if (ret == CL_VIRUS) {
                viruses_found++;
            }
            funmap(*ctx->fmap);
        }
        *ctx->fmap = map;
    } else {
        /* Since the above is moderately costly all in all,
         * do the old stuff if there's no relative offsets. */

        if (troot) {
            cli_targetinfo(&info, 7, map);
            ret = cli_ac_caloff(troot, &tmdata, &info);
            if (ret)
                goto done;
        }

        while (1) {
            size_t len = MIN(map->pgsz, map->len - at);
            buff       = fmap_need_off_once(map, at, len);
            at += len;
            if (!buff || !len || state.out_pos + len > state.out_len) {
                /* flush if error/EOF, or too little buffer space left */
                if ((ofd != -1) && (write(ofd, state.out, state.out_pos) == -1)) {
                    cli_errmsg("cli_scanscript: can't write to file %s\n", tmpname);
                    close(ofd);
                    ofd = -1;
                    /* we can continue to scan in memory */
                }
                /* when we flush the buffer also scan */
                if (cli_scan_buff(state.out, state.out_pos, offset, ctx, CL_TYPE_TEXT_ASCII, mdata) == CL_VIRUS) {
                    if (SCAN_ALLMATCHES)
                        viruses_found++;
                    else {
                        ret = CL_VIRUS;
                        break;
                    }
                }
                if (ctx->scanned)
                    *ctx->scanned += state.out_pos / CL_COUNT_PRECISION;
                offset += state.out_pos;
                /* carry over maxpatlen from previous buffer */
                if (state.out_pos > maxpatlen)
                    memmove(state.out, state.out + state.out_pos - maxpatlen, maxpatlen);
                text_normalize_reset(&state);
                state.out_pos = maxpatlen;
            }
            if (!len)
                break;
            if (!buff || text_normalize_buffer(&state, buff, len) != len) {
                cli_dbgmsg("cli_scanscript: short read during normalizing\n");
            }
        }
    }

    if (ret != CL_VIRUS || SCAN_ALLMATCHES) {
        if ((ret = cli_exp_eval(ctx, troot, &tmdata, NULL, NULL)) == CL_VIRUS)
            viruses_found++;
        if (ret != CL_VIRUS || SCAN_ALLMATCHES)
            if ((ret = cli_exp_eval(ctx, groot, &gmdata, NULL, NULL)) == CL_VIRUS)
                viruses_found++;
    }

done:
    cli_targetinfo_destroy(&info);

    if (NULL != normalized) {
        free(normalized);
    }

    if (tmdata_initialized) {
        cli_ac_freedata(&tmdata);
    }

    if (gmdata_initialized) {
        cli_ac_freedata(&gmdata);
    }

    if (ofd != -1)
        close(ofd);
    if (tmpname != NULL) {
        if (!ctx->engine->keeptmp)
            cli_unlink(tmpname);
        free(tmpname);
    }

    if (viruses_found)
        return CL_VIRUS;

    return ret;
}

static cl_error_t cli_scanhtml_utf16(cli_ctx *ctx)
{
    char *tempname, *decoded;
    const char *buff;
    cl_error_t ret = CL_CLEAN;
    int fd, bytes;
    size_t at   = 0;
    fmap_t *map = *ctx->fmap;

    cli_dbgmsg("in cli_scanhtml_utf16()\n");

    if (!(tempname = cli_gentemp_with_prefix(ctx->sub_tmpdir, "html-utf16-tmp")))
        return CL_EMEM;

    if ((fd = open(tempname, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR)) < 0) {
        cli_errmsg("cli_scanhtml_utf16: Can't create file %s\n", tempname);
        free(tempname);
        return CL_EOPEN;
    }

    cli_dbgmsg("cli_scanhtml_utf16: using tempfile %s\n", tempname);

    while (at < map->len) {
        bytes = MIN(map->len - at, map->pgsz * 16);
        if (!(buff = fmap_need_off_once(map, at, bytes))) {
            close(fd);
            cli_unlink(tempname);
            free(tempname);
            return CL_EREAD;
        }
        at += bytes;
        decoded = cli_utf16toascii(buff, bytes);
        if (decoded) {
            if (write(fd, decoded, bytes / 2) == -1) {
                cli_errmsg("cli_scanhtml_utf16: Can't write to file %s\n", tempname);
                free(decoded);
                close(fd);
                cli_unlink(tempname);
                free(tempname);
                return CL_EWRITE;
            }
            free(decoded);
        }
    }

    *ctx->fmap = fmap(fd, 0, 0, NULL);
    if (*ctx->fmap) {
        ret = cli_scanhtml(ctx);
        funmap(*ctx->fmap);
    } else
        cli_errmsg("cli_scanhtml_utf16: fmap of %s failed\n", tempname);

    *ctx->fmap = map;
    close(fd);

    if (!ctx->engine->keeptmp) {
        if (cli_unlink(tempname))
            ret = CL_EUNLINK;
    } else
        cli_dbgmsg("cli_scanhtml_utf16: Decoded HTML data saved in %s\n", tempname);
    free(tempname);

    return ret;
}

static cl_error_t cli_scanole2(cli_ctx *ctx)
{
    char *dir          = NULL;
    cl_error_t ret     = CL_CLEAN;
    struct uniq *files = NULL;
    int has_vba = 0, has_xlm = 0, has_macros = 0, viruses_found = 0;

    cli_dbgmsg("in cli_scanole2()\n");

    if (ctx->engine->maxreclevel && ctx->recursion >= ctx->engine->maxreclevel) {
        ret = CL_EMAXREC;
        goto done;
    }

    /* generate the temporary directory */
    if (NULL == (dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "ole2-tmp"))) {
        ret = CL_EMEM;
        goto done;
    }

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("OLE2: Can't create temporary directory %s\n", dir);
        free(dir);
        dir = NULL;
        ret = CL_ETMPDIR;
        goto done;
    }

    ret = cli_ole2_extract(dir, ctx, &files, &has_vba, &has_xlm);
    if (ret != CL_CLEAN && ret != CL_VIRUS) {
        cli_dbgmsg("OLE2: %s\n", cl_strerror(ret));
        goto done;
    }
    if (CL_VIRUS == ret) {
        viruses_found++;
        if (!SCAN_ALLMATCHES) {
            goto done;
        }
    }

    if (has_vba && files) {
        ctx->recursion++;

        ret = cli_vba_scandir(dir, ctx, files, &has_macros);
        if (CL_VIRUS == ret) {
            viruses_found++;
            if (!SCAN_ALLMATCHES) {
                ctx->recursion--;
                goto done;
            }
        }

        ret = cli_vba_scandir_new(dir, ctx, files, &has_macros);
        if (CL_VIRUS == ret) {
            viruses_found++;
            if (!SCAN_ALLMATCHES) {
                ctx->recursion--;
                goto done;
            }
        }

        ctx->recursion--;
    }

    if (CL_VIRUS == ret) {
        viruses_found++;
        if (!SCAN_ALLMATCHES) {
            goto done;
        }
    }

    if (has_xlm && files) {
        ctx->recursion++;

        ret = cli_xlm_scandir(dir, ctx, files);
        if (CL_VIRUS == ret) {
            viruses_found++;
            if (!SCAN_ALLMATCHES) {
                ctx->recursion--;
                goto done;
            }
        }

        ctx->recursion--;
    }

    if ((has_xlm || has_vba) && files) {
        ctx->recursion++;

        if (CL_VIRUS == cli_magic_scan_dir(dir, ctx)) {
            viruses_found++;
            if (!SCAN_ALLMATCHES) {
                ctx->recursion--;
                goto done;
            }
        }

        ctx->recursion--;
    }

done:
    if (files) {
        uniq_free(files);
    }

    if (NULL != dir) {
        if (!ctx->engine->keeptmp)
            cli_rmdirs(dir);
        free(dir);
    }

    if (viruses_found > 0) {
        ret = CL_VIRUS;
    }

    return ret;
}

static cl_error_t cli_scantar(cli_ctx *ctx, unsigned int posix)
{
    char *dir;
    cl_error_t ret = CL_CLEAN;

    cli_dbgmsg("in cli_scantar()\n");

    /* generate temporary directory */
    if (!(dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "tar-tmp")))
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_errmsg("Tar: Can't create temporary directory %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    ret = cli_untar(dir, posix, ctx);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    return ret;
}

static cl_error_t cli_scanscrenc(cli_ctx *ctx)
{
    char *tempname;
    cl_error_t ret = CL_CLEAN;

    cli_dbgmsg("in cli_scanscrenc()\n");

    if (!(tempname = cli_gentemp_with_prefix(ctx->sub_tmpdir, "screnc-tmp")))
        return CL_EMEM;

    if (mkdir(tempname, 0700)) {
        cli_dbgmsg("CHM: Can't create temporary directory %s\n", tempname);
        free(tempname);
        return CL_ETMPDIR;
    }

    if (html_screnc_decode(*ctx->fmap, tempname))
        ret = cli_magic_scan_dir(tempname, ctx);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(tempname);

    free(tempname);
    return ret;
}

static cl_error_t cli_scanriff(cli_ctx *ctx)
{
    cl_error_t ret = CL_CLEAN;

    if (cli_check_riff_exploit(ctx) == 2)
        ret = cli_append_virus(ctx, "Heuristics.Exploit.W32.MS05-002");

    return ret;
}

static cl_error_t cli_scancryptff(cli_ctx *ctx)
{
    cl_error_t ret = CL_CLEAN, ndesc;
    unsigned int i;
    const unsigned char *src;
    unsigned char *dest = NULL;
    char *tempfile;
    size_t pos;
    size_t bread;

    /* Skip the CryptFF file header */
    pos = 0x10;

    if ((dest = (unsigned char *)cli_malloc(FILEBUFF)) == NULL) {
        cli_dbgmsg("CryptFF: Can't allocate memory\n");
        return CL_EMEM;
    }

    if (!(tempfile = cli_gentemp_with_prefix(ctx->sub_tmpdir, "cryptff"))) {
        free(dest);
        return CL_EMEM;
    }

    if ((ndesc = open(tempfile, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR)) < 0) {
        cli_errmsg("CryptFF: Can't create file %s\n", tempfile);
        free(dest);
        free(tempfile);
        return CL_ECREAT;
    }

    for (; (src = fmap_need_off_once_len(*ctx->fmap, pos, FILEBUFF, &bread)) && bread; pos += bread) {
        for (i = 0; i < bread; i++)
            dest[i] = src[i] ^ (unsigned char)0xff;
        if (cli_writen(ndesc, dest, bread) == (size_t)-1) {
            cli_dbgmsg("CryptFF: Can't write to descriptor %d\n", ndesc);
            free(dest);
            close(ndesc);
            free(tempfile);
            return CL_EWRITE;
        }
    }

    free(dest);

    cli_dbgmsg("CryptFF: Scanning decrypted data\n");

    if ((ret = cli_magic_scan_desc(ndesc, tempfile, ctx, NULL)) == CL_VIRUS)
        cli_dbgmsg("CryptFF: Infected with %s\n", cli_get_last_virus(ctx));

    close(ndesc);

    if (ctx->engine->keeptmp)
        cli_dbgmsg("CryptFF: Decompressed data saved in %s\n", tempfile);
    else if (cli_unlink(tempfile))
        ret = CL_EUNLINK;

    free(tempfile);
    return ret;
}

static cl_error_t cli_scanpdf(cli_ctx *ctx, off_t offset)
{
    cl_error_t ret;
    char *dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "pdf-tmp");

    if (!dir)
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("Can't create temporary directory for PDF file %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    ret = cli_pdf(dir, ctx, offset);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    return ret;
}

static cl_error_t cli_scantnef(cli_ctx *ctx)
{
    cl_error_t ret;
    char *dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "tnef-tmp");

    if (!dir)
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("Can't create temporary directory for tnef file %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    ret = cli_tnef(dir, ctx);

    if (ret == CL_CLEAN)
        ret = cli_magic_scan_dir(dir, ctx);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    return ret;
}

static cl_error_t cli_scanuuencoded(cli_ctx *ctx)
{
    cl_error_t ret;
    char *dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "uuencoded-tmp");

    if (!dir)
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("Can't create temporary directory for uuencoded file %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    ret = cli_uuencode(dir, *ctx->fmap);

    if (ret == CL_CLEAN)
        ret = cli_magic_scan_dir(dir, ctx);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    return ret;
}

static cl_error_t cli_scanmail(cli_ctx *ctx)
{
    char *dir;
    cl_error_t ret;
    unsigned int viruses_found = 0;

    cli_dbgmsg("Starting cli_scanmail(), recursion = %u\n", ctx->recursion);

    /* generate the temporary directory */
    if (!(dir = cli_gentemp_with_prefix(ctx->sub_tmpdir, "mail-tmp")))
        return CL_EMEM;

    if (mkdir(dir, 0700)) {
        cli_dbgmsg("Mail: Can't create temporary directory %s\n", dir);
        free(dir);
        return CL_ETMPDIR;
    }

    /*
     * Extract the attachments into the temporary directory
     */
    if ((ret = cli_mbox(dir, ctx))) {
        if (ret == CL_VIRUS && SCAN_ALLMATCHES)
            viruses_found++;
        else {
            if (!ctx->engine->keeptmp)
                cli_rmdirs(dir);
            free(dir);
            return ret;
        }
    }

    ret = cli_magic_scan_dir(dir, ctx);

    if (!ctx->engine->keeptmp)
        cli_rmdirs(dir);

    free(dir);
    if (viruses_found)
        return CL_VIRUS;
    return ret;
}

static cl_error_t cli_scan_structured(cli_ctx *ctx)
{
    char buf[8192];
    size_t result          = 0;
    unsigned int cc_count  = 0;
    unsigned int ssn_count = 0;
    int done               = 0;
    fmap_t *map;
    size_t pos = 0;
    int (*ccfunc)(const unsigned char *buffer, size_t length, int cc_only);
    int (*ssnfunc)(const unsigned char *buffer, size_t length);
    unsigned int viruses_found = 0;

    if (ctx == NULL)
        return CL_ENULLARG;

    map = *ctx->fmap;

    if (ctx->engine->min_cc_count == 1)
        ccfunc = dlp_has_cc;
    else
        ccfunc = dlp_get_cc_count;

    switch (SCAN_HEURISTIC_STRUCTURED_SSN_NORMAL | SCAN_HEURISTIC_STRUCTURED_SSN_STRIPPED) {
        case (CL_SCAN_HEURISTIC_STRUCTURED_SSN_NORMAL | CL_SCAN_HEURISTIC_STRUCTURED_SSN_STRIPPED):
            if (ctx->engine->min_ssn_count == 1)
                ssnfunc = dlp_has_ssn;
            else
                ssnfunc = dlp_get_ssn_count;
            break;

        case CL_SCAN_HEURISTIC_STRUCTURED_SSN_NORMAL:
            if (ctx->engine->min_ssn_count == 1)
                ssnfunc = dlp_has_normal_ssn;
            else
                ssnfunc = dlp_get_normal_ssn_count;
            break;

        case CL_SCAN_HEURISTIC_STRUCTURED_SSN_STRIPPED:
            if (ctx->engine->min_ssn_count == 1)
                ssnfunc = dlp_has_stripped_ssn;
            else
                ssnfunc = dlp_get_stripped_ssn_count;
            break;

        default:
            ssnfunc = NULL;
    }

    while (!done && ((result = fmap_readn(map, buf, pos, 8191)) > 0) && (result != (size_t)-1)) {
        pos += result;
        if ((cc_count += ccfunc((const unsigned char *)buf, result,
                                (ctx->options->heuristic & CL_SCAN_HEURISTIC_STRUCTURED_CC) ? 1 : 0)) >= ctx->engine->min_cc_count) {
            done = 1;
        }

        if (ssnfunc && ((ssn_count += ssnfunc((const unsigned char *)buf, result)) >= ctx->engine->min_ssn_count)) {
            done = 1;
        }
    }

    if (cc_count != 0 && cc_count >= ctx->engine->min_cc_count) {
        cli_dbgmsg("cli_scan_structured: %u credit card numbers detected\n", cc_count);
        if (CL_VIRUS == cli_append_virus(ctx, "Heuristics.Structured.CreditCardNumber")) {
            if (SCAN_ALLMATCHES) {
                viruses_found++;
            } else {
                return CL_VIRUS;
            }
        }
    }

    if (ssn_count != 0 && ssn_count >= ctx->engine->min_ssn_count) {
        cli_dbgmsg("cli_scan_structured: %u social security numbers detected\n", ssn_count);
        if (CL_VIRUS == cli_append_virus(ctx, "Heuristics.Structured.SSN")) {
            if (SCAN_ALLMATCHES) {
                viruses_found++;
            } else {
                return CL_VIRUS;
            }
        }
    }

    if (viruses_found)
        return CL_VIRUS;
    return CL_CLEAN;
}

static cl_error_t cli_scanembpe(cli_ctx *ctx, off_t offset)
{
    cl_error_t ret = CL_CLEAN;
    int fd;
    size_t bytes;
    size_t size = 0;
    size_t todo;
    const char *buff;
    char *tmpname;
    fmap_t *map = *ctx->fmap;
    unsigned int corrupted_input;

    tmpname = cli_gentemp_with_prefix(ctx->sub_tmpdir, "embedded-pe");
    if (!tmpname)
        return CL_EMEM;

    if ((fd = open(tmpname, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR)) < 0) {
        cli_errmsg("cli_scanembpe: Can't create file %s\n", tmpname);
        free(tmpname);
        return CL_ECREAT;
    }

    todo = map->len - offset;
    while (1) {
        bytes = MIN(todo, map->pgsz);
        if (!bytes)
            break;

        if (!(buff = fmap_need_off_once(map, offset + size, bytes))) {
            close(fd);
            if (!ctx->engine->keeptmp) {
                if (cli_unlink(tmpname)) {
                    free(tmpname);
                    return CL_EUNLINK;
                }
            }
            free(tmpname);
            return CL_EREAD;
        }
        size += bytes;
        todo -= bytes;

        if (cli_checklimits("cli_scanembpe", ctx, size, 0, 0) != CL_CLEAN)
            break;

        if (cli_writen(fd, buff, bytes) != bytes) {
            cli_dbgmsg("cli_scanembpe: Can't write to temporary file\n");
            close(fd);
            if (!ctx->engine->keeptmp) {
                if (cli_unlink(tmpname)) {
                    free(tmpname);
                    return CL_EUNLINK;
                }
            }
            free(tmpname);
            return CL_EWRITE;
        }
    }

    ctx->recursion++;
    corrupted_input      = ctx->corrupted_input;
    ctx->corrupted_input = 1;
    ret                  = cli_magic_scan_desc(fd, tmpname, ctx, NULL);
    ctx->corrupted_input = corrupted_input;
    if (ret == CL_VIRUS) {
        cli_dbgmsg("cli_scanembpe: Infected with %s\n", cli_get_last_virus(ctx));
        close(fd);
        if (!ctx->engine->keeptmp) {
            if (cli_unlink(tmpname)) {
                free(tmpname);
                ctx->recursion--;
                return CL_EUNLINK;
            }
        }
        free(tmpname);
        ctx->recursion--;
        return CL_VIRUS;
    }
    ctx->recursion--;

    close(fd);
    if (!ctx->engine->keeptmp) {
        if (cli_unlink(tmpname)) {
            free(tmpname);
            return CL_EUNLINK;
        }
    }
    free(tmpname);

    /* intentionally ignore possible errors from cli_magic_scan_desc */
    return CL_CLEAN;
}

#if defined(_WIN32) || defined(C_LINUX) || defined(C_DARWIN)
#define PERF_MEASURE
#endif

#ifdef PERF_MEASURE

static struct
{
    enum perfev id;
    const char *name;
    enum ev_type type;
} perf_events[] = {
    {PERFT_SCAN, "full scan", ev_time},
    {PERFT_PRECB, "prescan cb", ev_time},
    {PERFT_POSTCB, "postscan cb", ev_time},
    {PERFT_CACHE, "cache", ev_time},
    {PERFT_FT, "filetype", ev_time},
    {PERFT_CONTAINER, "container", ev_time},
    {PERFT_SCRIPT, "script", ev_time},
    {PERFT_PE, "pe", ev_time},
    {PERFT_RAW, "raw", ev_time},
    {PERFT_RAWTYPENO, "raw container", ev_time},
    {PERFT_MAP, "map", ev_time},
    {PERFT_BYTECODE, "bytecode", ev_time},
    {PERFT_KTIME, "kernel", ev_int},
    {PERFT_UTIME, "user", ev_int}};

static void get_thread_times(uint64_t *kt, uint64_t *ut)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    ULARGE_INTEGER kl, ul;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
        *kt = *ut = 0;
        return;
    }
    kl.LowPart  = k.dwLowDateTime;
    kl.HighPart = k.dwHighDateTime;
    ul.LowPart  = u.dwLowDateTime;
    ul.HighPart = u.dwHighDateTime;
    *kt         = kl.QuadPart / 10;
    *ut         = ul.QuadPart / 10;
#else
    struct tms tbuf;
    if (times(&tbuf) != ((clock_t)-1)) {
        clock_t tck = sysconf(_SC_CLK_TCK);
        *kt         = ((uint64_t)1000000) * tbuf.tms_stime / tck;
        *ut         = ((uint64_t)1000000) * tbuf.tms_utime / tck;
    } else {
        *kt = *ut = 0;
    }
#endif
}

static inline void perf_init(cli_ctx *ctx)
{
    uint64_t kt, ut;
    unsigned i;

    if (!SCAN_DEV_COLLECT_PERF_INFO)
        return;

    ctx->perf = cli_events_new(PERFT_LAST);
    for (i = 0; i < sizeof(perf_events) / sizeof(perf_events[0]); i++) {
        if (cli_event_define(ctx->perf, perf_events[i].id, perf_events[i].name,
                             perf_events[i].type, multiple_sum) == -1)
            continue;
    }
    cli_event_time_start(ctx->perf, PERFT_SCAN);
    get_thread_times(&kt, &ut);
    cli_event_int(ctx->perf, PERFT_KTIME, -kt);
    cli_event_int(ctx->perf, PERFT_UTIME, -ut);
}

static inline void perf_done(cli_ctx *ctx)
{
    char timestr[512];
    char *p;
    unsigned i;
    uint64_t kt, ut;
    char *pend;
    cli_events_t *perf = ctx->perf;

    if (!perf)
        return;

    p     = timestr;
    pend  = timestr + sizeof(timestr) - 1;
    *pend = 0;

    cli_event_time_stop(perf, PERFT_SCAN);
    get_thread_times(&kt, &ut);
    cli_event_int(perf, PERFT_KTIME, kt);
    cli_event_int(perf, PERFT_UTIME, ut);

    for (i = 0; i < sizeof(perf_events) / sizeof(perf_events[0]); i++) {
        union ev_val val;
        unsigned count;

        cli_event_get(perf, perf_events[i].id, &val, &count);
        if (p < pend)
            p += snprintf(p, pend - p, "%s: %d.%03ums, ", perf_events[i].name,
                          (signed)(val.v_int / 1000),
                          (unsigned)(val.v_int % 1000));
    }
    *p = 0;
    cli_infomsg(ctx, "performance: %s\n", timestr);

    cli_events_free(perf);
    ctx->perf = NULL;
}

static inline void perf_start(cli_ctx *ctx, int id)
{
    cli_event_time_start(ctx->perf, id);
}

static inline void perf_stop(cli_ctx *ctx, int id)
{
    cli_event_time_stop(ctx->perf, id);
}

static inline void perf_nested_start(cli_ctx *ctx, int id, int nestedid)
{
    cli_event_time_nested_start(ctx->perf, id, nestedid);
}

static inline void perf_nested_stop(cli_ctx *ctx, int id, int nestedid)
{
    cli_event_time_nested_stop(ctx->perf, id, nestedid);
}

#else
static inline void perf_init(cli_ctx *ctx)
{
    UNUSEDPARAM(ctx);
}
static inline void perf_start(cli_ctx *ctx, int id)
{
    UNUSEDPARAM(ctx);
    UNUSEDPARAM(id);
}
static inline void perf_stop(cli_ctx *ctx, int id)
{
    UNUSEDPARAM(ctx);
    UNUSEDPARAM(id);
}
static inline void perf_nested_start(cli_ctx *ctx, int id, int nestedid)
{
    UNUSEDPARAM(ctx);
    UNUSEDPARAM(id);
    UNUSEDPARAM(nestedid);
}
static inline void perf_nested_stop(cli_ctx *ctx, int id, int nestedid)
{
    UNUSEDPARAM(ctx);
    UNUSEDPARAM(id);
    UNUSEDPARAM(nestedid);
}
static inline void perf_done(cli_ctx *ctx)
{
    UNUSEDPARAM(ctx);
}
#endif

/**
 * @brief Perform raw scan of current fmap.
 *
 * @param ctx       Current scan context.
 * @param type      File type
 * @param typercg   Enable type recognition (file typing scan results).
 *                  If 0, will be a regular ac-mode scan.
 * @param dettype   [out] If typercg enabled and scan detects HTML or MAIL types,
 *                  will output HTML or MAIL types after performing HTML/MAIL scans
 * @param refhash   Hash of current fmap
 * @return cl_error_t
 */
static cl_error_t scanraw(cli_ctx *ctx, cli_file_t type, uint8_t typercg, cli_file_t *dettype, unsigned char *refhash)
{
    cl_error_t ret = CL_CLEAN, nret = CL_CLEAN;
    struct cli_matched_type *ftoffset = NULL, *fpt;
    struct cli_exe_info peinfo;
    unsigned int acmode = AC_SCAN_VIR, break_loop = 0;
    fmap_t *map = *ctx->fmap;
#if HAVE_JSON
    struct json_object *parent_property = NULL;
#else
    void *parent_property = NULL;
#endif

    if (ctx->engine->maxreclevel && ctx->recursion >= ctx->engine->maxreclevel) {
        cli_check_blockmax(ctx, CL_EMAXREC);
        return CL_EMAXREC;
    }

    if ((typercg) &&
        // We should also omit bzips, but DMG's may be detected in bzips. (type != CL_TYPE_BZ) &&        /* Omit BZ files because they can contain portions of original files like zip file entries that cause invalid extractions and lots of warnings. Decompress first, then scan! */
        (type != CL_TYPE_GZ) &&        /* Omit GZ files because they can contain portions of original files like zip file entries that cause invalid extractions and lots of warnings. Decompress first, then scan! */
        (type != CL_TYPE_GPT) &&       /* Omit GPT files because it's an image format that we can extract and scan manually. */
        (type != CL_TYPE_CPIO_OLD) &&  /* Omit CPIO_OLD files because it's an image format that we can extract and scan manually. */
        (type != CL_TYPE_ZIP) &&       /* Omit ZIP files because it'll detect each zip file entry as SFXZIP, which is a waste. We'll extract it and then scan. */
        (type != CL_TYPE_OLD_TAR) &&   /* Omit OLD TAR files because it's a raw archive format that we can extract and scan manually. */
        (type != CL_TYPE_POSIX_TAR)) { /* Omit POSIX TAR files because it's a raw archive format that we can extract and scan manually. */
        /*
         * Enable file type recognition scan mode if requested, except for some some problematic types (above).
         */
        acmode |= AC_SCAN_FT;
    }

    perf_start(ctx, PERFT_RAW);
    ret = cli_scan_fmap(ctx, type == CL_TYPE_TEXT_ASCII ? CL_TYPE_ANY : type, 0, &ftoffset, acmode, NULL, refhash);
    perf_stop(ctx, PERFT_RAW);

    // TODO I think this causes embedded file extraction to stop when a
    // signature has matched in cli_scan_fmap, which wouldn't be what
    // we want if allmatch is specified.
    if (ret >= CL_TYPENO) {
        perf_nested_start(ctx, PERFT_RAWTYPENO, PERFT_SCAN);
        ctx->recursion++;
        fpt = ftoffset;

        while (fpt) {
            /* set current level as container AFTER recursing */
            cli_set_container(ctx, fpt->type, map->len);
            if (fpt->offset > 0) {
                /*
                 * Scan embedded file types.
                 */
#if HAVE_JSON
                if (SCAN_COLLECT_METADATA && ctx->wrkproperty) {
                    json_object *arrobj;

                    parent_property = ctx->wrkproperty;
                    if (!json_object_object_get_ex(parent_property, "EmbeddedObjects", &arrobj)) {
                        arrobj = json_object_new_array();
                        if (NULL == arrobj) {
                            cli_errmsg("scanraw: no memory for json properties object\n");
                            nret = CL_EMEM;
                            break;
                        }
                        json_object_object_add(parent_property, "EmbeddedObjects", arrobj);
                    }
                    ctx->wrkproperty = json_object_new_object();
                    if (NULL == ctx->wrkproperty) {
                        cli_errmsg("scanraw: no memory for json properties object\n");
                        nret = CL_EMEM;
                        break;
                    }
                    json_object_array_add(arrobj, ctx->wrkproperty);

                    ret = cli_jsonstr(ctx->wrkproperty, "FileType", cli_ftname(fpt->type));
                    if (ret != CL_SUCCESS) {
                        cli_errmsg("scanraw: failed to add string to json object\n");
                        nret = CL_EMEM;
                        break;
                    }

                    ret = cli_jsonint64(ctx->wrkproperty, "Offset", (int64_t)fpt->offset);
                    if (ret != CL_SUCCESS) {
                        cli_errmsg("scanraw: failed to add int to json object\n");
                        nret = CL_EMEM;
                        break;
                    }
                }
#endif
                switch (fpt->type) {
                    case CL_TYPE_MHTML:
                        if (SCAN_PARSE_MAIL && (DCONF_MAIL & MAIL_CONF_MBOX)) {
                            cli_dbgmsg("MHTML signature found at %u\n", (unsigned int)fpt->offset);
                            nret = ret = cli_scanmail(ctx);
                        }
                        break;

                    case CL_TYPE_XDP:
                        if (SCAN_PARSE_PDF && (DCONF_DOC & DOC_CONF_PDF)) {
                            cli_dbgmsg("XDP signature found at %u\n", (unsigned int)fpt->offset);
                            nret = ret = cli_scanxdp(ctx);
                        }
                        break;
                    case CL_TYPE_XML_WORD:
                        if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_MSXML)) {
                            cli_dbgmsg("XML-WORD signature found at %u\n", (unsigned int)fpt->offset);
                            nret = ret = cli_scanmsxml(ctx);
                        }
                        break;
                    case CL_TYPE_XML_XL:
                        if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_MSXML)) {
                            cli_dbgmsg("XML-XL signature found at %u\n", (unsigned int)fpt->offset);
                            nret = ret = cli_scanmsxml(ctx);
                        }
                        break;
                    case CL_TYPE_XML_HWP:
                        if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_HWP)) {
                            cli_dbgmsg("XML-HWP signature found at %u\n", (unsigned int)fpt->offset);
                            nret = ret = cli_scanhwpml(ctx);
                        }
                        break;
                    case CL_TYPE_RARSFX:
                        if (type != CL_TYPE_RAR && have_rar && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_RAR)) {
                            const char *filepath = NULL;
                            int fd               = -1;

                            char *tmpname = NULL;
                            int tmpfd     = -1;
                            size_t csize  = map->len - fpt->offset; /* not precise */

                            cli_set_container(ctx, CL_TYPE_RAR, csize);
                            cli_dbgmsg("RAR/RAR-SFX signature found at %u\n", (unsigned int)fpt->offset);

#ifdef _WIN32
                            if ((fpt->offset != 0) || (SCAN_UNPRIVILEGED) || (NULL == ctx->sub_filepath) || (0 != _access_s(ctx->sub_filepath, R_OK))) {
#else
                            if ((fpt->offset != 0) || (SCAN_UNPRIVILEGED) || (NULL == ctx->sub_filepath) || (0 != access(ctx->sub_filepath, R_OK))) {
#endif
                                /*
                                 * If map is not file-backed, or offset is not at the start of the file...
                                 * ...have to dump to file for scanrar.
                                 */
                                nret = fmap_dump_to_file(map, ctx->sub_filepath, ctx->sub_tmpdir, &tmpname, &tmpfd, fpt->offset, fpt->offset + csize);
                                if (nret != CL_SUCCESS) {
                                    cli_dbgmsg("scanraw: failed to generate temporary file.\n");
                                    ret        = nret;
                                    break_loop = 1;
                                    break;
                                }
                                filepath = tmpname;
                                fd       = tmpfd;
                            } else {
                                /* Use the original file and file descriptor. */
                                filepath = ctx->sub_filepath;
                                fd       = fmap_fd(map);
                            }

                            /* scan file */
                            nret = cli_scanrar(filepath, fd, ctx);

                            if ((NULL == tmpname) && (CL_EOPEN == nret)) {
                                /*
                                 * Failed to open the file using the original filename.
                                 * Try writing the file descriptor to a temp file and try again.
                                 */
                                nret = fmap_dump_to_file(map, ctx->sub_filepath, ctx->sub_tmpdir, &tmpname, &tmpfd, fpt->offset, fpt->offset + csize);
                                if (nret != CL_SUCCESS) {
                                    cli_dbgmsg("scanraw: failed to generate temporary file.\n");
                                    ret        = nret;
                                    break_loop = 1;
                                    break;
                                }
                                filepath = tmpname;
                                fd       = tmpfd;

                                /* try to scan again */
                                nret = cli_scanrar(filepath, fd, ctx);
                            }

                            if (tmpfd != -1) {
                                /* If dumped tempfile, need to cleanup */
                                close(tmpfd);
                                if (!ctx->engine->keeptmp) {
                                    if (cli_unlink(tmpname)) {
                                        ret = nret = CL_EUNLINK;
                                        break_loop = 1;
                                    }
                                }
                            }

                            if (tmpname != NULL) {
                                free(tmpname);
                            }
                        }
                        break;

                    case CL_TYPE_EGGSFX:
                        if (type != CL_TYPE_EGG && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_EGG)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_EGG, csize);
                            cli_dbgmsg("EGG/EGG-SFX signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanegg(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_ZIPSFX:
                        if (type != CL_TYPE_ZIP && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ZIP)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_ZIP, csize);
                            cli_dbgmsg("ZIP/ZIP-SFX signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_unzip_single(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_CABSFX:
                        if (type != CL_TYPE_MSCAB && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CAB)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_MSCAB, csize);
                            cli_dbgmsg("CAB/CAB-SFX signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanmscab(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_ARJSFX:
                        if (type != CL_TYPE_ARJ && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ARJ)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_ARJ, csize);
                            cli_dbgmsg("ARJ-SFX signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanarj(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_7ZSFX:
                        if (type != CL_TYPE_7Z && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_7Z)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_7Z, csize);
                            cli_dbgmsg("7Zip-SFX signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_7unz(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_ISO9660:
                        if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ISO9660)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_ISO9660, csize);
                            cli_dbgmsg("ISO9660 signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scaniso(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_NULSFT:
                        if (SCAN_PARSE_ARCHIVE && type == CL_TYPE_MSEXE && (DCONF_ARCH & ARCH_CONF_NSIS) &&
                            fpt->offset > 4) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_NULSFT, csize);
                            cli_dbgmsg("NSIS signature found at %u\n", (unsigned int)fpt->offset - 4);
                            nret = cli_scannulsft(ctx, fpt->offset - 4);
                        }
                        break;

                    case CL_TYPE_AUTOIT:
                        if (SCAN_PARSE_ARCHIVE && type == CL_TYPE_MSEXE && (DCONF_ARCH & ARCH_CONF_AUTOIT)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_AUTOIT, csize);
                            cli_dbgmsg("AUTOIT signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanautoit(ctx, fpt->offset + 23);
                        }
                        break;

                    case CL_TYPE_ISHIELD_MSI:
                        if (SCAN_PARSE_ARCHIVE && type == CL_TYPE_MSEXE && (DCONF_ARCH & ARCH_CONF_ISHIELD)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_ISHIELD_MSI, csize);
                            cli_dbgmsg("ISHIELD-MSI signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanishield_msi(ctx, fpt->offset + 14);
                        }
                        break;

                    case CL_TYPE_DMG:
                        if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_DMG)) {
                            cli_dbgmsg("DMG signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scandmg(ctx);
                        }
                        break;

                    case CL_TYPE_MBR:
                        if (SCAN_PARSE_ARCHIVE) {
                            int iret = cli_mbr_check2(ctx, 0);
                            if ((iret == CL_TYPE_GPT) && (DCONF_ARCH & ARCH_CONF_GPT)) {
                                cli_dbgmsg("Recognized GUID Partition Table file\n");
                                cli_set_container(ctx, CL_TYPE_GPT, map->len);
                                cli_dbgmsg("GPT signature found at %u\n", (unsigned int)fpt->offset);
                                nret = cli_scangpt(ctx, 0);
                            } else if ((iret == CL_CLEAN) && (DCONF_ARCH & ARCH_CONF_MBR)) {
                                cli_dbgmsg("MBR signature found at %u\n", (unsigned int)fpt->offset);
                                nret = cli_scanmbr(ctx, 0);
                            }
                        }
                        break;

                    case CL_TYPE_PDF:
                        if (type != CL_TYPE_PDF && SCAN_PARSE_PDF && (DCONF_DOC & DOC_CONF_PDF)) {
                            size_t csize = map->len - fpt->offset; /* not precise */
                            cli_set_container(ctx, CL_TYPE_PDF, csize);
                            cli_dbgmsg("PDF signature found at %u\n", (unsigned int)fpt->offset);
                            nret = cli_scanpdf(ctx, fpt->offset);
                        }
                        break;

                    case CL_TYPE_MSEXE:
                        if (SCAN_PARSE_PE && (type == CL_TYPE_MSEXE || type == CL_TYPE_ZIP || type == CL_TYPE_MSOLE2) && ctx->dconf->pe) {
                            uint64_t curr_len = map->len;
                            size_t csize      = map->len - fpt->offset; /* not precise */
                            /* CL_ENGINE_MAX_EMBEDDED_PE */
                            if (curr_len > ctx->engine->maxembeddedpe) {
                                cli_dbgmsg("scanraw: MaxEmbeddedPE exceeded\n");
                                break;
                            }
                            cli_set_container(ctx, CL_TYPE_MSEXE, csize);

                            cli_exe_info_init(&peinfo, fpt->offset);
                            // TODO We could probably substitute in a quicker
                            // method of determining whether a PE file exists
                            // at this offset.
                            if (cli_peheader(map, &peinfo, CLI_PEHEADER_OPT_NONE, NULL) != 0) {
                                /* Despite failing, peinfo memory may have been allocated and must be freed. */
                                cli_exe_info_destroy(&peinfo);
                            } else {
                                cli_dbgmsg("*** Detected embedded PE file at %u ***\n",
                                           (unsigned int)fpt->offset);

                                /* Immediately free up peinfo allocated memory, prior to any recursion */
                                cli_exe_info_destroy(&peinfo);

                                nret       = cli_scanembpe(ctx, fpt->offset);
                                break_loop = 1; /* we can stop here and other
                                                 * embedded executables will
                                                 * be found recursively
                                                 * through the above call
                                                 */
                                // TODO This method of embedded PE extraction
                                // is kinda gross in that:
                                //   - if you have an executable that contains
                                //     20 other exes, the bytes associated with
                                //     the last exe will have been included in
                                //     hash computations and things 20 times
                                //     (as overlay data to the previously
                                //     extracted exes).
                                //   - if you have a signed embedded exe, it
                                //     will fail to validate after extraction
                                //     bc it has overlay data, which is a
                                //     violation of the Authenticode spec.
                                //   - this method of extraction is subject to
                                //     the recursion limit, which is fairly
                                //     low by default (I think 16)
                                //
                                // It'd be awesome if we could compute the PE
                                // size from the PE header and just extract
                                // that.
                            }
                        }
                        break;

                    default:
                        cli_warnmsg("scanraw: Type %u not handled in fpt loop\n", fpt->type);
                }
            }

            if (nret == CL_VIRUS || nret == CL_EMEM || break_loop)
                break;

            fpt = fpt->next;

#if HAVE_JSON
            if (NULL != parent_property) {
                ctx->wrkproperty = (struct json_object *)(parent_property);
                parent_property  = NULL;
            }
#endif
        }

        if (nret != CL_VIRUS)
            /*
             * Now run the other file type parsers that may rely on file type
             * recognition to determine the actual file type.
             */
            switch (ret) {
                case CL_TYPE_HTML:
                    /* bb#11196 - autoit script file misclassified as HTML */
                    if (cli_get_container_intermediate(ctx, -2) == CL_TYPE_AUTOIT) {
                        ret = CL_TYPE_TEXT_ASCII;
                    } else if (SCAN_PARSE_HTML &&
                               (type == CL_TYPE_TEXT_ASCII ||
                                type == CL_TYPE_GIF) && /* Scan GIFs for embedded HTML/Javascript */
                               (DCONF_DOC & DOC_CONF_HTML)) {
                        *dettype = CL_TYPE_HTML;
                        nret     = cli_scanhtml(ctx);
                    }
                    break;

                case CL_TYPE_MAIL:
                    cli_set_container(ctx, CL_TYPE_MAIL, map->len);
                    if (SCAN_PARSE_MAIL && type == CL_TYPE_TEXT_ASCII && (DCONF_MAIL & MAIL_CONF_MBOX)) {
                        *dettype = CL_TYPE_MAIL;
                        nret     = cli_scanmail(ctx);
                    }
                    break;

                default:
                    break;
            }
        perf_nested_stop(ctx, PERFT_RAWTYPENO, PERFT_SCAN);
        ctx->recursion--;
        ret = nret;
    }

#if HAVE_JSON
    if (NULL != parent_property) {
        ctx->wrkproperty = (struct json_object *)(parent_property);
    }
#endif

    while (ftoffset) {
        fpt      = ftoffset;
        ftoffset = ftoffset->next;
        free(fpt);
    }

    if (ret == CL_VIRUS)
        cli_dbgmsg("%s found\n", cli_get_last_virus(ctx));

    return ret;
}

static void emax_reached(cli_ctx *ctx)
{
    fmap_t **ctx_fmap = ctx->fmap;
    if (!ctx_fmap)
        return;
    while (*ctx_fmap) {
        fmap_t *map          = *ctx_fmap;
        map->dont_cache_flag = 1;
        ctx_fmap--;
    }
    cli_dbgmsg("emax_reached: marked parents as non cacheable\n");
}

#define LINESTR(x) #x
#define LINESTR2(x) LINESTR(x)
#define __AT__ " at line " LINESTR2(__LINE__)

static cl_error_t dispatch_prescan_callback(clcb_pre_scan cb, cli_ctx *ctx, const char *filetype, bitset_t *old_hook_lsig_matches, void *parent_property, unsigned char *hash, size_t hashed_size, int *run_cleanup)
{
    cl_error_t res = CL_CLEAN;

    UNUSEDPARAM(parent_property);
    UNUSEDPARAM(hash);
    UNUSEDPARAM(hashed_size);

    *run_cleanup = 0;

    if (cb) {
        perf_start(ctx, PERFT_PRECB);
        switch (cb(fmap_fd(*ctx->fmap), filetype, ctx->cb_ctx)) {
            case CL_BREAK:
                cli_dbgmsg("dispatch_prescan_callback: file whitelisted by callback\n");
                perf_stop(ctx, PERFT_PRECB);
                ctx->hook_lsig_matches = old_hook_lsig_matches;
                /* returns CL_CLEAN */
                *run_cleanup = 1;
                break;
            case CL_VIRUS:
                cli_dbgmsg("dispatch_prescan_callback: file blacklisted by callback\n");
                cli_append_virus(ctx, "Detected.By.Callback");
                perf_stop(ctx, PERFT_PRECB);
                ctx->hook_lsig_matches = old_hook_lsig_matches;
                *run_cleanup           = 1;
                res                    = CL_VIRUS;
                break;
            case CL_CLEAN:
                break;
            default:
                cli_warnmsg("dispatch_prescan_callback: ignoring bad return code from callback\n");
        }

        perf_stop(ctx, PERFT_PRECB);
    }

    return res;
}

cl_error_t cli_magic_scan(cli_ctx *ctx, cli_file_t type)
{
    cl_error_t ret = CL_CLEAN;
    cl_error_t cb_retcode;
    cli_file_t dettype = 0;
    uint8_t typercg    = 1;
    size_t hashed_size;
    unsigned char *hash = NULL;
    bitset_t *old_hook_lsig_matches;
    const char *filetype;
    int cache_clean = 0, res;
    int run_cleanup = 0;
#if HAVE_JSON
    struct json_object *parent_property = NULL;
#else
    void *parent_property = NULL;
#endif

    char *old_temp_path = NULL;
    char *new_temp_path = NULL;

    if (!ctx->engine) {
        cli_errmsg("CRITICAL: engine == NULL\n");
        ret = CL_ENULLARG;
        goto early_ret;
    }

    if (!(ctx->engine->dboptions & CL_DB_COMPILED)) {
        cli_errmsg("CRITICAL: engine not compiled\n");
        ret = CL_EMALFDB;
        goto early_ret;
    }

    if (ctx->engine->maxreclevel && ctx->recursion > ctx->engine->maxreclevel) {
        cli_dbgmsg("cli_magic_scan: Archive recursion limit exceeded (%u, max: %u)\n", ctx->recursion, ctx->engine->maxreclevel);
        emax_reached(ctx);
        cli_check_blockmax(ctx, CL_EMAXREC);
        ret = CL_CLEAN;
        goto early_ret;
    }

    if ((*ctx->fmap)->len <= 5) {
        cli_dbgmsg("cli_magic_scandesc: File is too too small (%zu bytes), ignoring.\n", (*ctx->fmap)->len);
        ret = CL_CLEAN;
        goto early_ret;
    }

    if (cli_updatelimits(ctx, (*ctx->fmap)->len) != CL_CLEAN) {
        emax_reached(ctx);
        ret = CL_CLEAN;
        cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
        goto early_ret;
    }

    if (ctx->engine->keeptmp) {
        char *fmap_basename = NULL;
        /*
         * Keep-temp enabled, so create a sub-directory to provide extraction directory recursion.
         */
        if ((NULL != (*ctx->fmap)->name) &&
            (CL_SUCCESS == cli_basename((*ctx->fmap)->name, strlen((*ctx->fmap)->name), &fmap_basename))) {
            /*
             * The fmap has a name, lets include it in the new sub-directory.
             */
            new_temp_path = cli_gentemp_with_prefix(ctx->sub_tmpdir, fmap_basename);
            free(fmap_basename);
            if (NULL == new_temp_path) {
                cli_errmsg("cli_magic_scan: Failed to generate temp directory name.\n");
                ret = CL_EMEM;
                goto early_ret;
            }
        } else {
            /*
             * The fmap has no name or we failed to get the basename.
             */
            new_temp_path = cli_gentemp(ctx->sub_tmpdir);
            if (NULL == new_temp_path) {
                cli_errmsg("cli_magic_scan: Failed to generate temp directory name.\n");
                ret = CL_EMEM;
                goto early_ret;
            }
        }

        old_temp_path   = ctx->sub_tmpdir;
        ctx->sub_tmpdir = new_temp_path;

        if (mkdir(ctx->sub_tmpdir, 0700)) {
            cli_errmsg("cli_magic_scan: Can't create tmp sub-directory for scan: %s.\n", ctx->sub_tmpdir);
            ret = CL_EACCES;
            goto early_ret;
        }
    }

    hash        = (*ctx->fmap)->maphash;
    hashed_size = (*ctx->fmap)->len;

    old_hook_lsig_matches = ctx->hook_lsig_matches;
    if (type == CL_TYPE_PART_ANY) {
        typercg = 0;
    }

    /*
     * Perform file typing from the start of the file.
     */
    perf_start(ctx, PERFT_FT);
    if ((type == CL_TYPE_ANY) || type == CL_TYPE_PART_ANY) {
        type = cli_determine_fmap_type(*ctx->fmap, ctx->engine, type);
    }
    perf_stop(ctx, PERFT_FT);
    if (type == CL_TYPE_ERROR) {
        cli_dbgmsg("cli_magic_scan: cli_determine_fmap_type returned CL_TYPE_ERROR\n");
        ret = CL_EREAD;
        cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
        goto early_ret;
    }
    filetype = cli_ftname(type);

#if HAVE_JSON
    if (SCAN_COLLECT_METADATA) {
        /*
         * Create JSON object to record metadata during the scan.
         */
        if (NULL == ctx->properties) {
            ctx->properties = json_object_new_object();
            if (NULL == ctx->properties) {
                cli_errmsg("cli_magic_scan: no memory for json properties object\n");
                ret = CL_EMEM;
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }
            ctx->wrkproperty = ctx->properties;

            ret = cli_jsonstr(ctx->properties, "Magic", "CLAMJSONv0");
            if (ret != CL_SUCCESS) {
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }
            ret = cli_jsonstr(ctx->properties, "RootFileType", filetype);
            if (ret != CL_SUCCESS) {
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }

        } else {
            json_object *arrobj;

            parent_property = ctx->wrkproperty;
            if (!json_object_object_get_ex(parent_property, "ContainedObjects", &arrobj)) {
                arrobj = json_object_new_array();
                if (NULL == arrobj) {
                    cli_errmsg("cli_magic_scan: no memory for json properties object\n");
                    ret = CL_EMEM;
                    cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                    goto early_ret;
                }
                json_object_object_add(parent_property, "ContainedObjects", arrobj);
            }
            ctx->wrkproperty = json_object_new_object();
            if (NULL == ctx->wrkproperty) {
                cli_errmsg("cli_magic_scan: no memory for json properties object\n");
                ret = CL_EMEM;
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }
            json_object_array_add(arrobj, ctx->wrkproperty);
        }

        if ((*ctx->fmap)->name) {
            ret = cli_jsonstr(ctx->wrkproperty, "FileName", (*ctx->fmap)->name);
            if (ret != CL_SUCCESS) {
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }
        }
        if (ctx->sub_filepath) {
            ret = cli_jsonstr(ctx->wrkproperty, "FilePath", ctx->sub_filepath);
            if (ret != CL_SUCCESS) {
                cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
                goto early_ret;
            }
        }
        ret = cli_jsonstr(ctx->wrkproperty, "FileType", filetype);
        if (ret != CL_SUCCESS) {
            cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
            goto early_ret;
        }
        ret = cli_jsonint(ctx->wrkproperty, "FileSize", (*ctx->fmap)->len);
        if (ret != CL_SUCCESS) {
            cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
            goto early_ret;
        }
    }
#endif

    ret = dispatch_prescan_callback(ctx->engine->cb_pre_cache, ctx, filetype, old_hook_lsig_matches, parent_property, hash, hashed_size, &run_cleanup);
    if (run_cleanup) {
        if (ret == CL_VIRUS) {
            ret = cli_checkfp(ctx);
            goto done;
        } else {
            ret = CL_CLEAN;
            goto done;
        }
    }

    /*
     * Check if we've already scanned this file before.
     */
    perf_start(ctx, PERFT_CACHE);
    if (!(SCAN_COLLECT_METADATA))
        res = cache_check(hash, ctx);
    else
        res = CL_VIRUS;

#if HAVE_JSON
    if (SCAN_COLLECT_METADATA /* ctx.options->general & CL_SCAN_GENERAL_COLLECT_METADATA && ctx->wrkproperty != NULL */) {
        char hashstr[33];
        snprintf(hashstr, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                 hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
                 hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);

        ret = cli_jsonstr(ctx->wrkproperty, "FileMD5", hashstr);
        if (ctx->engine->engine_options & ENGINE_OPTIONS_DISABLE_CACHE)
            memset(hash, 0, 16);
        if (ret != CL_SUCCESS) {
            cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
            goto early_ret;
        }
    }
#endif

    if (res != CL_VIRUS) {
        perf_stop(ctx, PERFT_CACHE);
        cli_dbgmsg("cli_magic_scan: returning %d %s (no post, no cache)\n", ret, __AT__);
        goto early_ret;
    }

    perf_stop(ctx, PERFT_CACHE);
    ctx->hook_lsig_matches = NULL;

    if (!((ctx->options->general & ~CL_SCAN_GENERAL_ALLMATCHES) || (ctx->options->parse) || (ctx->options->heuristic) || (ctx->options->mail) || (ctx->options->dev)) || (ctx->recursion == ctx->engine->maxreclevel)) { /* raw mode (stdin, etc.) or last level of recursion */
        if (ctx->recursion == ctx->engine->maxreclevel) {
            cli_check_blockmax(ctx, CL_EMAXREC);
            cli_dbgmsg("cli_magic_scan: Hit recursion limit, only scanning raw file\n");
        } else
            cli_dbgmsg("cli_magic_scan: Raw mode: No support for special files\n");

        ret = dispatch_prescan_callback(ctx->engine->cb_pre_scan, ctx, filetype, old_hook_lsig_matches, parent_property, hash, hashed_size, &run_cleanup);
        if (run_cleanup) {
            if (ret == CL_VIRUS) {
                ret = cli_checkfp(ctx);
            }
            goto done;
        }

        if ((ret = cli_scan_fmap(ctx, CL_TYPE_ANY, 0, NULL, AC_SCAN_VIR, NULL, hash)) == CL_VIRUS)
            cli_dbgmsg("cli_magic_scan: %s found in descriptor %d\n", cli_get_last_virus(ctx), fmap_fd(*ctx->fmap));
        else if (ret == CL_CLEAN) {
            if (ctx->recursion != ctx->engine->maxreclevel)
                cache_clean = 1; /* Only cache if limits are not reached */
            else
                emax_reached(ctx);
        }

        ctx->hook_lsig_matches = old_hook_lsig_matches;
        goto done;
    }

    ret = dispatch_prescan_callback(ctx->engine->cb_pre_scan, ctx, filetype, old_hook_lsig_matches, parent_property, hash, hashed_size, &run_cleanup);
    if (run_cleanup) {
        if (ret == CL_VIRUS) {
            ret = cli_checkfp(ctx);
        }
        goto done;
    }

#ifdef HAVE__INTERNAL__SHA_COLLECT
    if (!ctx->sha_collect && type == CL_TYPE_MSEXE)
        ctx->sha_collect = 1;
#endif

    ctx->hook_lsig_matches = cli_bitset_init();
    if (!ctx->hook_lsig_matches) {
        ctx->hook_lsig_matches = old_hook_lsig_matches;
        ret                    = CL_EMEM;
        goto done;
    }

    if (type != CL_TYPE_IGNORED && ctx->engine->sdb) {
        /*
         * If self protection mechanism enabled, do the scanraw() scan first
         * before extracting with a file type parser.
         */
        ret = scanraw(ctx, type, 0, &dettype, (ctx->engine->engine_options & ENGINE_OPTIONS_DISABLE_CACHE) ? NULL : hash);
        if (ret == CL_EMEM || ret == CL_VIRUS) {
            ret = cli_checkfp(ctx);
            cli_bitset_free(ctx->hook_lsig_matches);
            ctx->hook_lsig_matches = old_hook_lsig_matches;
            goto done;
        }
    }

    /*
     * Run the file type parsers that we normally use before the raw scan.
     */
    ctx->recursion++;
    perf_nested_start(ctx, PERFT_CONTAINER, PERFT_SCAN);
    /* set current level as container AFTER recursing */
    cli_set_container(ctx, type, (*ctx->fmap)->len);
    switch (type) {
        case CL_TYPE_IGNORED:
            break;

        case CL_TYPE_HWP3:
            if (SCAN_PARSE_HWP3 && (DCONF_DOC & DOC_CONF_HWP))
                ret = cli_scanhwp3(ctx);
            break;

        case CL_TYPE_HWPOLE2:
            if (SCAN_PARSE_OLE2 && (DCONF_ARCH & ARCH_CONF_OLE2))
                ret = cli_scanhwpole2(ctx);
            break;

        case CL_TYPE_XML_WORD:
            if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_MSXML))
                ret = cli_scanmsxml(ctx);
            break;

        case CL_TYPE_XML_XL:
            if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_MSXML))
                ret = cli_scanmsxml(ctx);
            break;

        case CL_TYPE_XML_HWP:
            if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_HWP))
                ret = cli_scanhwpml(ctx);
            break;

        case CL_TYPE_XDP:
            if (SCAN_PARSE_PDF && (DCONF_DOC & DOC_CONF_PDF))
                ret = cli_scanxdp(ctx);
            break;

        case CL_TYPE_RAR:
            if (have_rar && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_RAR)) {
                const char *filepath = NULL;
                int fd               = -1;

                char *tmpname = NULL;
                int tmpfd     = -1;

#ifdef _WIN32
                if ((SCAN_UNPRIVILEGED) || (NULL == ctx->sub_filepath) || (0 != _access_s(ctx->sub_filepath, R_OK))) {
#else
                if ((SCAN_UNPRIVILEGED) || (NULL == ctx->sub_filepath) || (0 != access(ctx->sub_filepath, R_OK))) {
#endif
                    /* If map is not file-backed have to dump to file for scanrar. */
                    ret = fmap_dump_to_file(*ctx->fmap, ctx->sub_filepath, ctx->sub_tmpdir, &tmpname, &tmpfd, 0, SIZE_MAX);
                    if (ret != CL_SUCCESS) {
                        cli_dbgmsg("cli_magic_scan: failed to generate temporary file.\n");
                        break;
                    }
                    filepath = tmpname;
                    fd       = tmpfd;
                } else {
                    /* Use the original file and file descriptor. */
                    filepath = ctx->sub_filepath;
                    fd       = fmap_fd(*ctx->fmap);
                }

                /* scan file */
                ret = cli_scanrar(filepath, fd, ctx);

                if ((NULL == tmpname) && (CL_EOPEN == ret)) {
                    /*
                     * Failed to open the file using the original filename.
                     * Try writing the file descriptor to a temp file and try again.
                     */
                    ret = fmap_dump_to_file(*ctx->fmap, ctx->sub_filepath, ctx->sub_tmpdir, &tmpname, &tmpfd, 0, SIZE_MAX);
                    if (ret != CL_SUCCESS) {
                        cli_dbgmsg("cli_magic_scan: failed to generate temporary file.\n");
                        break;
                    }
                    filepath = tmpname;
                    fd       = tmpfd;

                    /* try to scan again */
                    ret = cli_scanrar(filepath, fd, ctx);
                }

                if (tmpfd != -1) {
                    /* If dumped tempfile, need to cleanup */
                    close(tmpfd);
                    if (!ctx->engine->keeptmp) {
                        if (cli_unlink(tmpname)) {
                            ret = CL_EUNLINK;
                        }
                    }
                }

                if (tmpname != NULL) {
                    free(tmpname);
                }
            }
            break;

        case CL_TYPE_EGG:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_EGG))
                ret = cli_scanegg(ctx, 0);
            break;

        case CL_TYPE_OOXML_WORD:
        case CL_TYPE_OOXML_PPT:
        case CL_TYPE_OOXML_XL:
        case CL_TYPE_OOXML_HWP:
#if HAVE_JSON
            if (SCAN_PARSE_XMLDOCS && (DCONF_DOC & DOC_CONF_OOXML)) {
                if (SCAN_COLLECT_METADATA && (ctx->wrkproperty != NULL)) {
                    ret = cli_process_ooxml(ctx, type);

                    if (ret == CL_EMEM || ret == CL_ENULLARG) {
                        /* critical error */
                        break;
                    } else if (ret != CL_SUCCESS) {
                        /*
                         * non-critical return => allow for the CL_TYPE_ZIP scan to occur
                         * cli_process_ooxml other possible returns:
                         *   CL_ETIMEOUT, CL_EMAXSIZE, CL_EMAXFILES, CL_EPARSE,
                         *   CL_EFORMAT, CL_BREAK, CL_ESTAT
                         */
                        ret = CL_SUCCESS;
                    }
                }
            }
#endif
            /* fall-through */
        case CL_TYPE_ZIP:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ZIP))
                ret = cli_unzip(ctx);
            break;

        case CL_TYPE_GZ:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_GZ))
                ret = cli_scangzip(ctx);
            break;

        case CL_TYPE_BZ:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_BZ))
                ret = cli_scanbzip(ctx);
            break;

        case CL_TYPE_XZ:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_XZ))
                ret = cli_scanxz(ctx);
            break;

        case CL_TYPE_GPT:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_GPT))
                ret = cli_scangpt(ctx, 0);
            break;

        case CL_TYPE_APM:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_APM))
                ret = cli_scanapm(ctx);
            break;

        case CL_TYPE_ARJ:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ARJ))
                ret = cli_scanarj(ctx, 0);
            break;

        case CL_TYPE_NULSFT:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_NSIS))
                ret = cli_scannulsft(ctx, 0);
            break;

        case CL_TYPE_AUTOIT:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_AUTOIT))
                ret = cli_scanautoit(ctx, 23);
            break;

        case CL_TYPE_MSSZDD:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_SZDD))
                ret = cli_scanszdd(ctx);
            break;

        case CL_TYPE_MSCAB:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CAB))
                ret = cli_scanmscab(ctx, 0);
            break;

        case CL_TYPE_HTML:
            if (SCAN_PARSE_HTML && (DCONF_DOC & DOC_CONF_HTML))
                ret = cli_scanhtml(ctx);
            break;

        case CL_TYPE_HTML_UTF16:
            if (SCAN_PARSE_HTML && (DCONF_DOC & DOC_CONF_HTML))
                ret = cli_scanhtml_utf16(ctx);
            break;

        case CL_TYPE_SCRIPT:
            if ((DCONF_DOC & DOC_CONF_SCRIPT) && dettype != CL_TYPE_HTML)
                ret = cli_scanscript(ctx);
            break;

        case CL_TYPE_SWF:
            if (SCAN_PARSE_SWF && (DCONF_DOC & DOC_CONF_SWF))
                ret = cli_scanswf(ctx);
            break;

        case CL_TYPE_RTF:
            if (SCAN_PARSE_ARCHIVE && (DCONF_DOC & DOC_CONF_RTF))
                ret = cli_scanrtf(ctx);
            break;

        case CL_TYPE_MAIL:
            if (SCAN_PARSE_MAIL && (DCONF_MAIL & MAIL_CONF_MBOX))
                ret = cli_scanmail(ctx);
            break;

        case CL_TYPE_MHTML:
            if (SCAN_PARSE_MAIL && (DCONF_MAIL & MAIL_CONF_MBOX))
                ret = cli_scanmail(ctx);
            break;

        case CL_TYPE_TNEF:
            if (SCAN_PARSE_MAIL && (DCONF_MAIL & MAIL_CONF_TNEF))
                ret = cli_scantnef(ctx);
            break;

        case CL_TYPE_UUENCODED:
            if (DCONF_OTHER & OTHER_CONF_UUENC)
                ret = cli_scanuuencoded(ctx);
            break;

        case CL_TYPE_MSCHM:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CHM))
                ret = cli_scanmschm(ctx);
            break;

        case CL_TYPE_MSOLE2:
            if (SCAN_PARSE_OLE2 && (DCONF_ARCH & ARCH_CONF_OLE2))
                ret = cli_scanole2(ctx);
            break;

        case CL_TYPE_7Z:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_7Z))
                ret = cli_7unz(ctx, 0);
            break;

        case CL_TYPE_POSIX_TAR:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_TAR))
                ret = cli_scantar(ctx, 1);
            break;

        case CL_TYPE_OLD_TAR:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_TAR))
                ret = cli_scantar(ctx, 0);
            break;

        case CL_TYPE_CPIO_OLD:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CPIO))
                ret = cli_scancpio_old(ctx);
            break;

        case CL_TYPE_CPIO_ODC:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CPIO))
                ret = cli_scancpio_odc(ctx);
            break;

        case CL_TYPE_CPIO_NEWC:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CPIO))
                ret = cli_scancpio_newc(ctx, 0);
            break;

        case CL_TYPE_CPIO_CRC:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_CPIO))
                ret = cli_scancpio_newc(ctx, 1);
            break;

        case CL_TYPE_BINHEX:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_BINHEX))
                ret = cli_binhex(ctx);
            break;

        case CL_TYPE_SCRENC:
            if (DCONF_OTHER & OTHER_CONF_SCRENC)
                ret = cli_scanscrenc(ctx);
            break;

        case CL_TYPE_RIFF:
            if (SCAN_HEURISTICS && (DCONF_OTHER & OTHER_CONF_RIFF))
                ret = cli_scanriff(ctx);
            break;

        case CL_TYPE_GRAPHICS:
            /*
             * This case is for unhandled graphics types such as BMP, JPEG 2000, etc.
             *
             * Note: JPEG 2000 is a very different format from JPEG, JPEG/JFIF, JPEG/Exif, JPEG/SPIFF (1994, 1997)
             * JPEG 2000 is not handled by cli_scanjpeg or cli_parsejpeg.
             */
            break;

        case CL_TYPE_GIF:
            if (SCAN_HEURISTICS && SCAN_HEURISTIC_BROKEN_MEDIA && (DCONF_OTHER & OTHER_CONF_GIF))
                ret = cli_parsegif(ctx);
            break;

        case CL_TYPE_PNG:
            if (SCAN_HEURISTICS && (DCONF_OTHER & OTHER_CONF_PNG))
                ret = cli_parsepng(ctx); /* PNG parser detects a couple CVE's as well as Broken.Media */
            break;

        case CL_TYPE_JPEG:
            if (SCAN_HEURISTICS && (DCONF_OTHER & OTHER_CONF_JPEG))
                ret = cli_parsejpeg(ctx); /* JPG parser detects MS04-028 exploits as well as Broken.Media */
            break;

        case CL_TYPE_TIFF:
            if (SCAN_HEURISTICS && SCAN_HEURISTIC_BROKEN_MEDIA && (DCONF_OTHER & OTHER_CONF_TIFF) && ret != CL_VIRUS)
                ret = cli_parsetiff(ctx);
            break;

        case CL_TYPE_PDF: /* FIXMELIMITS: pdf should be an archive! */
            if (SCAN_PARSE_PDF && (DCONF_DOC & DOC_CONF_PDF))
                ret = cli_scanpdf(ctx, 0);
            break;

        case CL_TYPE_CRYPTFF:
            if (DCONF_OTHER & OTHER_CONF_CRYPTFF)
                ret = cli_scancryptff(ctx);
            break;

        case CL_TYPE_ELF:
            if (SCAN_PARSE_ELF && ctx->dconf->elf)
                ret = cli_scanelf(ctx);
            break;

        case CL_TYPE_MACHO:
            if (ctx->dconf->macho)
                ret = cli_scanmacho(ctx, NULL);
            break;

        case CL_TYPE_MACHO_UNIBIN:
            if (ctx->dconf->macho)
                ret = cli_scanmacho_unibin(ctx);
            break;

        case CL_TYPE_SIS:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_SIS))
                ret = cli_scansis(ctx);
            break;

        case CL_TYPE_XAR:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_XAR))
                ret = cli_scanxar(ctx);
            break;

        case CL_TYPE_PART_HFSPLUS:
            if (SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_HFSPLUS))
                ret = cli_scanhfsplus(ctx);
            break;

        case CL_TYPE_BINARY_DATA:
        case CL_TYPE_TEXT_UTF16BE:
            if (SCAN_HEURISTICS && (DCONF_OTHER & OTHER_CONF_MYDOOMLOG))
                ret = cli_check_mydoom_log(ctx);
            break;

        case CL_TYPE_TEXT_ASCII:
            if (SCAN_HEURISTIC_STRUCTURED && (DCONF_OTHER & OTHER_CONF_DLP))
                /* TODO: consider calling this from cli_scanscript() for
                 * a normalised text
                 */

                ret = cli_scan_structured(ctx);
            break;

        default:
            break;
    }
    perf_nested_stop(ctx, PERFT_CONTAINER, PERFT_SCAN);
    ctx->recursion--;

    /*
     * Perform the raw scan, which may include file type recognition signatures.
     */
    if (ret == CL_VIRUS && !SCAN_ALLMATCHES) {
        cli_bitset_free(ctx->hook_lsig_matches);
        ctx->hook_lsig_matches = old_hook_lsig_matches;
        goto done;
    }

    /* Disable type recognition for the raw scan for zip files larger than maxziptypercg */
    if (type == CL_TYPE_ZIP && SCAN_PARSE_ARCHIVE && (DCONF_ARCH & ARCH_CONF_ZIP)) {
        /* CL_ENGINE_MAX_ZIPTYPERCG */
        uint64_t curr_len = (*ctx->fmap)->len;
        if (curr_len > ctx->engine->maxziptypercg) {
            cli_dbgmsg("cli_magic_scan_desc: Not checking for embedded PEs (zip file > MaxZipTypeRcg)\n");
            typercg = 0;
        }
    }

    /* CL_TYPE_HTML: raw HTML files are not scanned, unless safety measure activated via DCONF */
    if (type != CL_TYPE_IGNORED && (type != CL_TYPE_HTML || !(SCAN_PARSE_HTML) || !(DCONF_DOC & DOC_CONF_HTML_SKIPRAW)) && !ctx->engine->sdb) {
        res = scanraw(ctx, type, typercg, &dettype, (ctx->engine->engine_options & ENGINE_OPTIONS_DISABLE_CACHE) ? NULL : hash);
        if (res != CL_CLEAN) {
            switch (res) {
                /* List of scan halts, runtime errors only! */
                case CL_EUNLINK:
                case CL_ESTAT:
                case CL_ESEEK:
                case CL_EWRITE:
                case CL_EDUP:
                case CL_ETMPFILE:
                case CL_ETMPDIR:
                case CL_EMEM:
                    cli_dbgmsg("Descriptor[%d]: scanraw error %s\n", fmap_fd(*ctx->fmap), cl_strerror(res));
                    cli_bitset_free(ctx->hook_lsig_matches);
                    ctx->hook_lsig_matches = old_hook_lsig_matches;
                    ret                    = res;
                    goto done;
                /* CL_VIRUS = malware found, check FP and report.
                 * Likewise, if the file was determined to be trusted, then we
                 * can also finish with the scan. (Ex: EXE with a valid
                 * Authenticode sig.) */
                case CL_VERIFIED:
                    // For now just conver CL_VERIFIED to CL_CLEAN, since
                    // CL_VERIFIED isn't used elsewhere
                    res = CL_CLEAN;
                    // Fall through
                case CL_VIRUS:
                    ret = res;
                    if (SCAN_ALLMATCHES)
                        break;
                    cli_bitset_free(ctx->hook_lsig_matches);
                    ctx->hook_lsig_matches = old_hook_lsig_matches;
                    goto done;
                /* The CL_ETIMEOUT "MAX" condition should set exceeds max flag and exit out quietly. */
                case CL_ETIMEOUT:
                    cli_check_blockmax(ctx, ret);
                    cli_bitset_free(ctx->hook_lsig_matches);
                    ctx->hook_lsig_matches = old_hook_lsig_matches;
                    cli_dbgmsg("Descriptor[%d]: Stopping after scanraw reached %s\n",
                               fmap_fd(*ctx->fmap), cl_strerror(res));
                    ret = CL_CLEAN;
                    goto done;
                /* All other "MAX" conditions should still fully scan the current file */
                case CL_EMAXREC:
                case CL_EMAXSIZE:
                case CL_EMAXFILES:
                    ret = res;
                    cli_dbgmsg("Descriptor[%d]: Continuing after scanraw reached %s\n",
                               fmap_fd(*ctx->fmap), cl_strerror(res));
                    break;
                /* Other errors must not block further scans below
                 * This specifically includes CL_EFORMAT & CL_EREAD & CL_EUNPACK
                 * Malformed/truncated files could report as any of these three.
                 */
                default:
                    ret = res;
                    cli_dbgmsg("Descriptor[%d]: Continuing after scanraw error %s\n",
                               fmap_fd(*ctx->fmap), cl_strerror(res));
            }
        }
    }

    /*
     * Now run the rest of the file type parsers.
     */
    ctx->recursion++;
    switch (type) {
        /* bytecode hooks triggered by a lsig must be a hook
         * called from one of the functions here */
        case CL_TYPE_TEXT_ASCII:
        case CL_TYPE_TEXT_UTF16BE:
        case CL_TYPE_TEXT_UTF16LE:
        case CL_TYPE_TEXT_UTF8:
            perf_nested_start(ctx, PERFT_SCRIPT, PERFT_SCAN);
            if ((DCONF_DOC & DOC_CONF_SCRIPT) && dettype != CL_TYPE_HTML && (ret != CL_VIRUS || SCAN_ALLMATCHES) && SCAN_PARSE_HTML)
                ret = cli_scanscript(ctx);
            if (SCAN_PARSE_MAIL && (DCONF_MAIL & MAIL_CONF_MBOX) && ret != CL_VIRUS && (cli_get_container(ctx, -1) == CL_TYPE_MAIL || dettype == CL_TYPE_MAIL)) {
                ret = cli_scan_fmap(ctx, CL_TYPE_MAIL, 0, NULL, AC_SCAN_VIR, NULL, NULL);
            }
            perf_nested_stop(ctx, PERFT_SCRIPT, PERFT_SCAN);
            break;
        /* Due to performance reasons all executables were first scanned
         * in raw mode. Now we will try to unpack them
         */
        case CL_TYPE_MSEXE:
            perf_nested_start(ctx, PERFT_PE, PERFT_SCAN);
            if (SCAN_PARSE_PE && ctx->dconf->pe) {
                unsigned int corrupted_input = ctx->corrupted_input;
                ret                          = cli_scanpe(ctx);
                ctx->corrupted_input         = corrupted_input;
            }
            perf_nested_stop(ctx, PERFT_PE, PERFT_SCAN);
            break;
        case CL_TYPE_ELF:
            perf_nested_start(ctx, PERFT_ELF, PERFT_SCAN);
            ret = cli_unpackelf(ctx);
            perf_nested_stop(ctx, PERFT_ELF, PERFT_SCAN);
            break;
        case CL_TYPE_MACHO:
        case CL_TYPE_MACHO_UNIBIN:
            perf_nested_start(ctx, PERFT_MACHO, PERFT_SCAN);
            ret = cli_unpackmacho(ctx);
            perf_nested_stop(ctx, PERFT_MACHO, PERFT_SCAN);
            break;
        case CL_TYPE_BINARY_DATA:
            ret = cli_scan_fmap(ctx, CL_TYPE_OTHER, 0, NULL, AC_SCAN_VIR, NULL, NULL);
            break;
        default:
            break;
    }

    ctx->recursion--;
    cli_bitset_free(ctx->hook_lsig_matches);
    ctx->hook_lsig_matches = old_hook_lsig_matches;

    switch (ret) {
        /* Limits exceeded */
        case CL_ETIMEOUT:
        case CL_EMAXREC:
        case CL_EMAXSIZE:
        case CL_EMAXFILES:
            cli_check_blockmax(ctx, ret);
            /* fall-through */
        /* Malformed file cases */
        case CL_EFORMAT:
        case CL_EREAD:
        case CL_EUNPACK:
            cli_dbgmsg("Descriptor[%d]: %s\n", fmap_fd(*ctx->fmap), cl_strerror(ret));
            ret = CL_CLEAN;
            goto done;
        case CL_CLEAN:
            cache_clean = 1;
            ret         = CL_CLEAN;
            goto done;
        default:
            goto done;
    }

done:

#if HAVE_JSON
    ctx->wrkproperty = (struct json_object *)(parent_property);
#endif

    if (ret == CL_CLEAN && ctx->found_possibly_unwanted) {
        cb_retcode = CL_VIRUS;
    } else {
        if (ret == CL_CLEAN && ctx->num_viruses != 0)
            cb_retcode = CL_VIRUS;
        else
            cb_retcode = ret;
    }

    cli_dbgmsg("cli_magic_scan_desc: returning %d %s\n", ret, __AT__);
    if (ctx->engine->cb_post_scan) {
        const char *virusname = NULL;
        perf_start(ctx, PERFT_POSTCB);
        if (cb_retcode == CL_VIRUS)
            virusname = cli_get_last_virus(ctx);
        switch (ctx->engine->cb_post_scan(fmap_fd(*ctx->fmap), cb_retcode, virusname, ctx->cb_ctx)) {
            case CL_BREAK:
                cli_dbgmsg("cli_magic_scan_desc: file whitelisted by post_scan callback\n");
                perf_stop(ctx, PERFT_POSTCB);
                ret = CL_CLEAN;
                break;
            case CL_VIRUS:
                cli_dbgmsg("cli_magic_scan_desc: file blacklisted by post_scan callback\n");
                cli_append_virus(ctx, "Detected.By.Callback");
                perf_stop(ctx, PERFT_POSTCB);
                if (ret != CL_VIRUS) {
                    ret = cli_checkfp(ctx);
                }
                break;
            case CL_CLEAN:
                break;
            default:
                cli_warnmsg("cli_magic_scan_desc: ignoring bad return code from post_scan callback\n");
        }
        perf_stop(ctx, PERFT_POSTCB);
    }
    if (cb_retcode == CL_CLEAN && cache_clean) {
        perf_start(ctx, PERFT_CACHE);
        if (!(SCAN_COLLECT_METADATA))
            cache_add(hash, hashed_size, ctx);
        perf_stop(ctx, PERFT_CACHE);
    }
    if (ret == CL_VIRUS && SCAN_ALLMATCHES) {
        ret = CL_CLEAN;
    }

early_ret:

    if ((ctx->engine->keeptmp) && (NULL != old_temp_path)) {
        /* Use rmdir to remove empty tmp subdirectories. If rmdir fails, it wasn't empty. */
        (void)rmdir(ctx->sub_tmpdir);

        free((void *)ctx->sub_tmpdir);
        ctx->sub_tmpdir = old_temp_path;
    }

#if HAVE_JSON
    if (NULL != parent_property) {
        ctx->wrkproperty = (struct json_object *)(parent_property);
    }
#endif

    return ret;
}

cl_error_t cli_magic_scan_desc_type(int desc, const char *filepath, cli_ctx *ctx, cli_file_t type, const char *name)
{
    STATBUF sb;
    cl_error_t status = CL_CLEAN;

    if (!ctx) {
        return CL_EARG;
    }

    const char *parent_filepath = ctx->sub_filepath;
    ctx->sub_filepath           = filepath;

#ifdef HAVE__INTERNAL__SHA_COLLECT
    if (ctx->sha_collect > 0)
        ctx->sha_collect = 0;
#endif
    cli_dbgmsg("in cli_magic_scan_desc_type (reclevel: %u/%u)\n", ctx->recursion, ctx->engine->maxreclevel);
    if (FSTAT(desc, &sb) == -1) {
        cli_errmsg("cli_magic_scan: Can't fstat descriptor %d\n", desc);

        status = CL_ESTAT;
        cli_dbgmsg("cli_magic_scan_desc_type: returning %d %s (no post, no cache)\n", status, __AT__);
        goto done;
    }
    if (sb.st_size <= 5) {
        cli_dbgmsg("Small data (%u bytes)\n", (unsigned int)sb.st_size);

        status = CL_CLEAN;
        cli_dbgmsg("cli_magic_scan_desc_type: returning %d %s (no post, no cache)\n", status, __AT__);
        goto done;
    }

    ctx->fmap++;
    perf_start(ctx, PERFT_MAP);
    if (!(*ctx->fmap = fmap(desc, 0, sb.st_size, name))) {
        cli_errmsg("CRITICAL: fmap() failed\n");
        ctx->fmap--;
        perf_stop(ctx, PERFT_MAP);

        status = CL_EMEM;
        cli_dbgmsg("cli_magic_scan_desc_type: returning %d %s (no post, no cache)\n", status, __AT__);
        goto done;
    }
    perf_stop(ctx, PERFT_MAP);

    status = cli_magic_scan(ctx, type);

    funmap(*ctx->fmap);
    ctx->fmap--;

done:
    ctx->sub_filepath = parent_filepath;

    return status;
}

cl_error_t cli_magic_scan_desc(int desc, const char *filepath, cli_ctx *ctx, const char *name)
{
    return cli_magic_scan_desc_type(desc, filepath, ctx, CL_TYPE_ANY, name);
}

cl_error_t cl_scandesc(int desc, const char *filename, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions)
{
    return cl_scandesc_callback(desc, filename, virname, scanned, engine, scanoptions, NULL);
}

/**
 * @brief   Scan an offset/length into a file map.
 *
 * Magic-scan some portion of an existing fmap.
 *
 * @param map       File map.
 * @param offset    Offset into file map.
 * @param length    Length from offset.
 * @param ctx       Scanning context structure.
 * @param type      CL_TYPE of data to be scanned.
 * @param name      (optional) Original name of the file (to set fmap name metadata)
 * @return int      CL_SUCCESS, or an error code.
 */
static cl_error_t magic_scan_nested_fmap_type(cl_fmap_t *map, size_t offset, size_t length, cli_ctx *ctx, cli_file_t type, const char *name)
{
    cl_error_t ret = CL_CLEAN;

    cli_dbgmsg("magic_scan_nested_fmap_type: [%zu, +%zu), [" STDi64 ", +%zu)\n",
               map->nested_offset, map->len,
               (int64_t)offset, length);
    if (offset >= map->len) {
        cli_dbgmsg("Invalid offset: %zu\n", offset);
        return CL_CLEAN;
    }

    if (!length)
        length = map->len - offset;
    if (length > map->len - offset) {
        cli_dbgmsg("Data truncated: %zu -> %zu\n",
                   length, map->len - offset);
        length = map->len - offset;
    }

    if (length <= 5) {
        cli_dbgmsg("Small data (%zu bytes)\n", length);
        return CL_CLEAN;
    }
    ctx->fmap++;
    *ctx->fmap = fmap_duplicate(map, offset, length, name);
    if (NULL == *ctx->fmap) {
        cli_dbgmsg("Failed to duplicate fmap for scan of fmap subsection\n");
        ctx->fmap--;
        return CL_CLEAN;
    }

    ret = cli_magic_scan(ctx, type);

    free_duplicate_fmap(*ctx->fmap); /* This fmap is just a duplicate. */
    *ctx->fmap = NULL;
    ctx->fmap--;

    return ret;
}

/* For map scans that may be forced to disk */
cl_error_t cli_magic_scan_nested_fmap_type(cl_fmap_t *map, size_t offset, size_t length, cli_ctx *ctx, cli_file_t type, const char *name)
{
    size_t old_off = map->nested_offset;
    size_t old_len = map->len;
    cl_error_t ret = CL_CLEAN;

    cli_dbgmsg("cli_magic_scan_nested_fmap_type: [%zu, +%zu)\n", offset, length);
    if (offset >= old_len) {
        cli_dbgmsg("Invalid offset: %zu\n", offset);
        return CL_CLEAN;
    }

    if (ctx->engine->engine_options & ENGINE_OPTIONS_FORCE_TO_DISK) {
        /* if this is forced to disk, then need to write the nested map and scan it */
        const uint8_t *mapdata = NULL;
        char *tempfile         = NULL;
        int fd                 = -1;
        size_t nread           = 0;

        /* Then check length */
        if (!length)
            length = old_len - offset;
        if (length > old_len - offset) {
            cli_dbgmsg("cli_magic_scan_nested_fmap_type: Data truncated: %zu -> %zu\n", length, old_len - offset);
            length = old_len - offset;
        }
        if (length <= 5) {
            cli_dbgmsg("cli_magic_scan_nested_fmap_type: Small data (%u bytes)\n", (unsigned int)length);
            return CL_CLEAN;
        }
        if (!CLI_ISCONTAINED(old_off, old_len, old_off + offset, length)) {
            cli_dbgmsg("cli_magic_scan_nested_fmap_type: map error occurred [%zu, %zu]\n", old_off, old_len);
            return CL_CLEAN;
        }

        /* Length checked, now get map */
        mapdata = fmap_need_off_once_len(map, offset, length, &nread);
        if (!mapdata || (nread != length)) {
            cli_errmsg("cli_magic_scan_nested_fmap_type: could not map sub-file\n");
            return CL_EMAP;
        }

        ret = cli_gentempfd(ctx->sub_tmpdir, &tempfile, &fd);
        if (ret != CL_SUCCESS) {
            return ret;
        }

        cli_dbgmsg("cli_magic_scan_nested_fmap_type: writing nested map content to temp file %s\n", tempfile);
        if (cli_writen(fd, mapdata, length) == (size_t)-1) {
            cli_errmsg("cli_magic_scan_nested_fmap_type: cli_writen error writing subdoc temporary file.\n");
            ret = CL_EWRITE;
        }

        /* scan the temp file */
        ret = cli_magic_scan_desc_type(fd, tempfile, ctx, type, name);

        /* remove the temp file, if needed */
        if (fd >= 0) {
            close(fd);
        }
        if (!ctx->engine->keeptmp) {
            if (cli_unlink(tempfile)) {
                cli_errmsg("cli_magic_scan_nested_fmap_type: error unlinking tempfile %s\n", tempfile);
                ret = CL_EUNLINK;
            }
        }
        free(tempfile);
    } else {
        /* Not forced to disk, use nested map */
        ret = magic_scan_nested_fmap_type(map, offset, length, ctx, type, name);
    }
    return ret;
}

cl_error_t cli_magic_scan_buff(const void *buffer, size_t length, cli_ctx *ctx, const char *name)
{
    cl_error_t ret;
    fmap_t *map = NULL;

    map = fmap_open_memory(buffer, length, name);
    if (!map) {
        return CL_EMAP;
    }

    ret = cli_magic_scan_nested_fmap_type(map, 0, length, ctx, CL_TYPE_ANY, name);

    funmap(map);

    return ret;
}

/**
 * @brief   The main function to initiate a scan of an fmap.
 *
 * @param map               File map.
 * @param filepath          (optional, recommended) filepath of the open file descriptor or file map.
 * @param[out] virname      Will be set to a statically allocated (i.e. needs not be freed) signature name if the scan matches against a signature.
 * @param[out] scanned      The number of bytes scanned.
 * @param engine            The scanning engine.
 * @param scanoptions       Scanning options.
 * @param[inout] context    An opaque context structure allowing the caller to record details about the sample being scanned.
 * @return int              CL_CLEAN, CL_VIRUS, or an error code if an error occured during the scan.
 */
static cl_error_t scan_common(cl_fmap_t *map, const char *filepath, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions, void *context)
{
    cli_ctx ctx;
    cl_error_t rc;

    char *target_basename = NULL;
    char *new_temp_prefix = NULL;
    size_t new_temp_prefix_len;
    char *new_temp_path = NULL;

    time_t current_time;
    struct tm tm_struct;
    fmap_t **fmap_head = NULL;

    if (NULL == map) {
        return CL_ENULLARG;
    }

    /* We have a limit of around 2GB (INT_MAX - 2). Enforce it here. */
    /* TODO: Large file support is large-ly untested. Remove this restriction
     * and test with a large set of large files of various types. libclamav's
     * integer type safety has come a long way since 2014, so it's possible
     * we could lift this restriction, but at least one of the parsers is
     * bound to behave badly with large files. */
    if ((size_t)(map->real_len) > (size_t)(INT_MAX - 2))
        return CL_CLEAN;

    memset(&ctx, '\0', sizeof(cli_ctx));
    ctx.engine  = engine;
    ctx.virname = virname;
    ctx.scanned = scanned;
    ctx.options = malloc(sizeof(struct cl_scan_options));
    memcpy(ctx.options, scanoptions, sizeof(struct cl_scan_options));
    ctx.found_possibly_unwanted = 0;
    ctx.containers              = cli_calloc(sizeof(cli_ctx_container), ctx.engine->maxreclevel + 2);
    if (!ctx.containers) {
        rc = CL_EMEM;
        goto done;
    }
    cli_set_container(&ctx, CL_TYPE_ANY, 0);
    ctx.dconf  = (struct cli_dconf *)engine->dconf;
    ctx.cb_ctx = context;
    fmap_head  = cli_calloc(sizeof(fmap_t *), ctx.engine->maxreclevel + 3);
    if (!fmap_head) {
        rc = CL_EMEM;
        goto done;
    }
    if (!(ctx.hook_lsig_matches = cli_bitset_init())) {
        rc = CL_EMEM;
        goto done;
    }

    /*
     * The first fmap in ctx.fmap must be NULL so we can fmap-- while not NULL.
     * But we need an fmap to be set so we can append viruses or report the
     * fmap's file descriptor in the virus found callback (like for deferred
     * low-seveerity alerts).
     */
    ctx.fmap  = fmap_head + 1;
    *ctx.fmap = map;

    perf_init(&ctx);

    if (ctx.engine->maxscantime != 0) {
        if (gettimeofday(&ctx.time_limit, NULL) == 0) {
            uint32_t secs  = ctx.engine->maxscantime / 1000;
            uint32_t usecs = (ctx.engine->maxscantime % 1000) * 1000;
            ctx.time_limit.tv_sec += secs;
            ctx.time_limit.tv_usec += usecs;
            if (ctx.time_limit.tv_usec >= 1000000) {
                ctx.time_limit.tv_usec -= 1000000;
                ctx.time_limit.tv_sec++;
            }
        } else {
            char buf[64];
            cli_dbgmsg("scan_common: gettimeofday error: %s\n", cli_strerror(errno, buf, 64));
        }
    }

    if (filepath != NULL) {
        ctx.target_filepath = strdup(filepath);
    }

    /*
     * Create a tmp sub-directory for the temp files generated by this scan.
     *
     * If keeptmp (LeaveTemporaryFiles / --leave-temps) is enabled, we'll include the
     *   basename in the tmp directory.
     * If keeptmp is not enabled, we'll just call it "scantemp".
     */
    current_time = time(NULL);

#ifdef _WIN32
    if (0 != localtime_s(&tm_struct, &current_time)) {
#else
    if (!localtime_r(&current_time, &tm_struct)) {
#endif
        cli_errmsg("scan_common: Failed to get local time.\n");
        rc = CL_ESTAT;
        goto done;
    }

    if ((ctx.engine->keeptmp) &&
        (NULL != ctx.target_filepath) &&
        (CL_SUCCESS == cli_basename(ctx.target_filepath, strlen(ctx.target_filepath), &target_basename))) {
        /* Include the basename in the temp directory */
        new_temp_prefix_len = strlen("YYYYMMDD_HHMMSS-") + strlen(target_basename);
        new_temp_prefix     = cli_calloc(1, new_temp_prefix_len + 1);
        if (!new_temp_prefix) {
            cli_errmsg("scan_common: Failed to allocate memory for temp directory name.\n");
            rc = CL_EMEM;
            goto done;
        }
        strftime(new_temp_prefix, new_temp_prefix_len, "%Y%m%d_%H%M%S-", &tm_struct);
        strcpy(new_temp_prefix + strlen("YYYYMMDD_HHMMSS-"), target_basename);
    } else {
        /* Just use date */
        new_temp_prefix_len = strlen("YYYYMMDD_HHMMSS-scantemp");
        new_temp_prefix     = cli_calloc(1, new_temp_prefix_len + 1);
        if (!new_temp_prefix) {
            cli_errmsg("scan_common: Failed to allocate memory for temp directory name.\n");
            rc = CL_EMEM;
            goto done;
        }
        strftime(new_temp_prefix, new_temp_prefix_len, "%Y%m%d_%H%M%S-scantemp", &tm_struct);
    }

    /* Place the new temp sub-directory within the configured temp directory */
    new_temp_path = cli_gentemp_with_prefix(ctx.engine->tmpdir, new_temp_prefix);
    free(new_temp_prefix);
    if (NULL == new_temp_path) {
        cli_errmsg("scan_common: Failed to generate temp directory name.\n");
        rc = CL_EMEM;
        goto done;
    }

    ctx.sub_tmpdir = new_temp_path;

    if (mkdir(ctx.sub_tmpdir, 0700)) {
        cli_errmsg("Can't create temporary directory for scan: %s.\n", ctx.sub_tmpdir);
        rc = CL_EACCES;
        goto done;
    }

    cli_logg_setup(&ctx);

    rc = cli_magic_scan(&ctx, CL_TYPE_ANY);

    if (rc == CL_CLEAN && ctx.found_possibly_unwanted) {
        cli_virus_found_cb(&ctx);
    }

#if HAVE_JSON
    if (ctx.options->general & CL_SCAN_GENERAL_COLLECT_METADATA && (ctx.properties != NULL)) {
        json_object *jobj;
        const char *jstring;

        /* set value of unique root object tag */
        if (json_object_object_get_ex(ctx.properties, "FileType", &jobj)) {
            enum json_type type;
            const char *jstr;

            type = json_object_get_type(jobj);
            if (type == json_type_string) {
                jstr = json_object_get_string(jobj);
                cli_jsonstr(ctx.properties, "RootFileType", jstr);
            }
        }

        /* serialize json properties to string */
#ifdef JSON_C_TO_STRING_NOSLASHESCAPE
        jstring = json_object_to_json_string_ext(ctx.properties, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
#else
        jstring = json_object_to_json_string_ext(ctx.properties, JSON_C_TO_STRING_PRETTY);
#endif
        if (NULL == jstring) {
            cli_errmsg("scan_common: no memory for json serialization.\n");
            rc = CL_EMEM;
        } else {
            int ret                   = CL_SUCCESS;
            struct cli_matcher *iroot = ctx.engine->root[13];
            cli_dbgmsg("%s\n", jstring);

            if ((rc != CL_VIRUS) || (ctx.options->general & CL_SCAN_GENERAL_ALLMATCHES)) {
                /* run bytecode preclass hook; generate fmap if needed for running hook */
                struct cli_bc_ctx *bc_ctx = cli_bytecode_context_alloc();
                if (!bc_ctx) {
                    cli_errmsg("scan_common: can't allocate memory for bc_ctx\n");
                    rc = CL_EMEM;
                } else {
                    cli_bytecode_context_setctx(bc_ctx, &ctx);
                    rc = cli_bytecode_runhook(&ctx, ctx.engine, bc_ctx, BC_PRECLASS, map);
                    cli_bytecode_context_destroy(bc_ctx);
                }

                /* backwards compatibility: scan the json string unless a virus was detected */
                if (rc != CL_VIRUS && (iroot->ac_lsigs || iroot->ac_patterns
#ifdef HAVE_PCRE
                                       || iroot->pcre_metas
#endif // HAVE_PCRE
                                       )) {
                    cli_dbgmsg("scan_common: running deprecated preclass bytecodes for target type 13\n");
                    ctx.options->general &= ~CL_SCAN_GENERAL_COLLECT_METADATA;
                    rc = cli_magic_scan_buff(jstring, strlen(jstring), &ctx, NULL);
                }
            }

            /* Invoke file props callback */
            if (ctx.engine->cb_file_props != NULL) {
                ret = ctx.engine->cb_file_props(jstring, rc, ctx.cb_ctx);
                if (ret != CL_SUCCESS)
                    rc = ret;
            }

            /* keeptmp file processing for file properties json string */
            if (ctx.engine->keeptmp) {
                int fd        = -1;
                char *tmpname = NULL;

                if ((ret = cli_newfilepathfd(ctx.sub_tmpdir, "metadata.json", &tmpname, &fd)) != CL_SUCCESS) {
                    cli_dbgmsg("scan_common: Can't create json properties file, ret = %i.\n", ret);
                } else {
                    if (cli_writen(fd, jstring, strlen(jstring)) == (size_t)-1)
                        cli_dbgmsg("scan_common: cli_writen error writing json properties file.\n");
                    else
                        cli_dbgmsg("json written to: %s\n", tmpname);
                }
                if (fd != -1)
                    close(fd);
                if (NULL != tmpname)
                    free(tmpname);
            }
        }
        cli_json_delobj(ctx.properties); /* frees all json memory */
    }
#endif // HAVE_JSON

    if (rc == CL_CLEAN) {
        if ((ctx.found_possibly_unwanted) ||
            ((ctx.num_viruses != 0) &&
             ((ctx.options->general & CL_SCAN_GENERAL_ALLMATCHES) ||
              (ctx.options->heuristic & CL_SCAN_HEURISTIC_EXCEEDS_MAX)))) {
            rc = CL_VIRUS;
        }
    }

    cli_logg_unsetup();

done:
    if (NULL != ctx.sub_tmpdir) {
        if (!ctx.engine->keeptmp) {
            (void)cli_rmdirs(ctx.sub_tmpdir);
        }
        free(ctx.sub_tmpdir);
    }

    if (NULL != target_basename) {
        free(target_basename);
    }

    if (NULL != ctx.target_filepath) {
        free(ctx.target_filepath);
    }

    if (NULL != ctx.perf) {
        perf_done(&ctx);
    }

    if (NULL != ctx.hook_lsig_matches) {
        cli_bitset_free(ctx.hook_lsig_matches);
    }

    if (NULL != fmap_head) {
        free(fmap_head);
    }

    if (NULL != ctx.containers) {
        free(ctx.containers);
    }

    if (NULL != ctx.options) {
        free(ctx.options);
    }

    return rc;
}

cl_error_t cl_scandesc_callback(int desc, const char *filename, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions, void *context)
{
    cl_error_t status = CL_SUCCESS;
    cl_fmap_t *map    = NULL;
    STATBUF sb;
    char *filename_base = NULL;

    if (FSTAT(desc, &sb) == -1) {
        cli_errmsg("cl_scandesc_callback: Can't fstat descriptor %d\n", desc);
        status = CL_ESTAT;
        goto done;
    }
    if (sb.st_size <= 5) {
        cli_dbgmsg("cl_scandesc_callback: File too small (" STDu64 " bytes), ignoring\n", (uint64_t)sb.st_size);
        status = CL_CLEAN;
        goto done;
    }
    if ((uint64_t)sb.st_size > engine->maxfilesize) {
        cli_dbgmsg("cl_scandesc_callback: File too large (" STDu64 " bytes), ignoring\n", (uint64_t)sb.st_size);
        if (scanoptions->heuristic & CL_SCAN_HEURISTIC_EXCEEDS_MAX) {
            engine->cb_virus_found(desc, "Heuristics.Limits.Exceeded", context);
            status = CL_VIRUS;
        } else {
            status = CL_CLEAN;
        }
        goto done;
    }

    if (NULL != filename) {
        (void)cli_basename(filename, strlen(filename), &filename_base);
    }

    if (NULL == (map = fmap(desc, 0, sb.st_size, filename_base))) {
        cli_errmsg("CRITICAL: fmap() failed\n");
        status = CL_EMEM;
        goto done;
    }

    status = scan_common(map, filename, virname, scanned, engine, scanoptions, context);

done:
    if (NULL != map) {
        funmap(map);
    }
    if (NULL != filename_base) {
        free(filename_base);
    }

    return status;
}

cl_error_t cl_scanmap_callback(cl_fmap_t *map, const char *filename, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions, void *context)
{
    if (map->real_len > engine->maxfilesize) {
        cli_dbgmsg("cl_scandesc_callback: File too large (%zu bytes), ignoring\n", map->real_len);
        if (scanoptions->heuristic & CL_SCAN_HEURISTIC_EXCEEDS_MAX) {
            engine->cb_virus_found(fmap_fd(map), "Heuristics.Limits.Exceeded", context);
            return CL_VIRUS;
        }
        return CL_CLEAN;
    }

    return scan_common(map, filename, virname, scanned, engine, scanoptions, context);
}

cl_error_t cli_found_possibly_unwanted(cli_ctx *ctx)
{
    if (cli_get_last_virus(ctx)) {
        cli_dbgmsg("found Possibly Unwanted: %s\n", cli_get_last_virus(ctx));
        if (SCAN_HEURISTIC_PRECEDENCE) {
            /* we found a heuristic match, don't scan further,
             * but consider it a virus. */
            cli_dbgmsg("cli_found_possibly_unwanted: CL_VIRUS\n");
            return CL_VIRUS;
        }
        /* heuristic scan isn't taking precedence, keep scanning.
         * If this is part of an archive, and
         * we find a real malware we report that instead of the
         * heuristic match */
        ctx->found_possibly_unwanted = 1;
    } else {
        cli_warnmsg("cli_found_possibly_unwanted called, but virname is not set\n");
    }
    emax_reached(ctx);
    return CL_CLEAN;
}

cl_error_t cli_magic_scan_file(const char *filename, cli_ctx *ctx, const char *original_name)
{
    int fd         = -1;
    cl_error_t ret = CL_EOPEN;

    /* internal version of cl_scanfile with arec/mrec preserved */
    fd = safe_open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        goto done;
    }

    ret = cli_magic_scan_desc(fd, filename, ctx, original_name);

done:
    if (fd >= 0) {
        close(fd);
    }
    return ret;
}

cl_error_t cl_scanfile(const char *filename, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions)
{
    return cl_scanfile_callback(filename, virname, scanned, engine, scanoptions, NULL);
}

cl_error_t cl_scanfile_callback(const char *filename, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, struct cl_scan_options *scanoptions, void *context)
{
    int fd;
    cl_error_t ret;
    const char *fname = cli_to_utf8_maybe_alloc(filename);

    if (!fname)
        return CL_EARG;

    if ((fd = safe_open(fname, O_RDONLY | O_BINARY)) == -1) {
        if (errno == EACCES) {
            return CL_EACCES;
        } else {
            return CL_EOPEN;
        }
    }

    if (fname != filename)
        free((char *)fname);

    ret = cl_scandesc_callback(fd, filename, virname, scanned, engine, scanoptions, context);
    close(fd);

    return ret;
}

/*
Local Variables:
   c-basic-offset: 4
End:
*/
