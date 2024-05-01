/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include<unistd.h>

#include "common/attributes.h"
#include "common/intops.h"

#include "input/input.h"
#include "input/demuxer.h"

#if defined(_WIN32) || defined(__OS2__)
#include <io.h>
#include <fcntl.h>
#ifdef __OS2__
#define _setmode setmode
#define _fileno fileno
#define _O_BINARY O_BINARY
#endif
#endif


struct DemuxerContext {
    DemuxerPriv *data;
    const Demuxer *impl;
    uint64_t priv_data[];
};

extern const Demuxer ivf_demuxer;
extern const Demuxer annexb_demuxer;
extern const Demuxer section5_demuxer;
static const Demuxer *const demuxers[] = {
    &ivf_demuxer,
    &annexb_demuxer,
    &section5_demuxer,
    NULL
};

static FILE *set_binary_mode(FILE *stream) {
  (void)stream;
#if defined(_WIN32) || defined(__OS2__)
  _setmode(_fileno(stream), _O_BINARY);
#endif
  return stream;
}


int input_open(DemuxerContext **const c_out,
               const char *const name, const char *const filename,
               unsigned fps[2], unsigned *const num_frames, unsigned timebase[2])
{
    const Demuxer *impl;
    DemuxerContext *c;
    int res, i;
    FILE *f;
    char *fname;
    char name_buff[16];

    if (name) {
        for (i = 0; demuxers[i]; i++) {
            if (!strcmp(demuxers[i]->name, name)) {
                impl = demuxers[i];
                break;
            }
        }
        if (!demuxers[i]) {
            fprintf(stderr, "Failed to find demuxer named \"%s\"\n", name);
            return DAV1D_ERR(ENOPROTOOPT);
        }
    } else {
        int probe_sz = 0;
        for (i = 0; demuxers[i]; i++)
            probe_sz = imax(probe_sz, demuxers[i]->probe_sz);
        uint8_t *const probe_data = malloc(probe_sz);
        if (!probe_data) {
            fprintf(stderr, "Failed to allocate memory\n");
            return DAV1D_ERR(ENOMEM);
        }

        if (!strcmp(filename, "-")) {
            fprintf(stderr, "david reading from stdin\n");

            set_binary_mode(stdin);

            char *template_name = "dav1d-XXXXXX";

            memset(name_buff, 0, sizeof(name_buff));

            // Copy the relevant information in the buffers
            memcpy(name_buff, template_name, 12);

            #if defined(_WIN32) || defined(__OS2__)

            int err = _mktemp_s( name_buff, 32 );

            if (err != 0) {
                fprintf(stderr, "Failed to make a temp file: %s\n", strerror(errno));
                return errno ? DAV1D_ERR(errno) : DAV1D_ERR(EIO);
            }

            f = fopen(name_buff, "w+b");
            printf("Reached here\n");

            #else

            int fd = mkstemp(name_buff);

            if (fd < 1) {
                fprintf(stderr, "Failed to make a temp file: %s\n", strerror(errno));
                return errno ? DAV1D_ERR(errno) : DAV1D_ERR(EIO);
            }

            f = fdopen(fd, "w+b");
            #endif

            if (!f) {
                fprintf(stderr, "Failed to open temporary file: %s\n", strerror(errno));
                if (!strcmp(filename, "-")) unlink(name_buff);
                return errno ? DAV1D_ERR(errno) : DAV1D_ERR(EIO);
            }

            size_t n, m;
            unsigned char buff[8192];
            do {
                n = fread(buff, 1, sizeof buff, stdin);
                if (n) m = fwrite(buff, 1, n, f);
                else   m = 0;
            } while ((n > 0) && (n == m));

            fname = name_buff;

            fseek(f, SEEK_SET, 0);
        } else {
            fname = (char *) filename;
            f = fopen(filename, "rb");
        }

        if (!f) {
            fprintf(stderr, "Failed to open input file %s: %s\n", fname, strerror(errno));

            return errno ? DAV1D_ERR(errno) : DAV1D_ERR(EIO);
        }
        res = !!fread(probe_data, 1, probe_sz, f);
        fclose(f);
        if (!res) {
            free(probe_data);
            if (!strcmp(filename, "-")) unlink(fname);
            fprintf(stderr, "Failed to read probe data\n");
            return errno ? DAV1D_ERR(errno) : DAV1D_ERR(EIO);
        }

        for (i = 0; demuxers[i]; i++) {
            if (demuxers[i]->probe(probe_data)) {
                impl = demuxers[i];
                break;
            }
        }
        free(probe_data);
        if (!demuxers[i]) {
            fprintf(stderr,
                    "Failed to probe demuxer for file %s\n",
                    fname);
            if (!strcmp(filename, "-")) unlink(fname);
            return DAV1D_ERR(ENOPROTOOPT);
        }
    }

    if (!(c = calloc(1, offsetof(DemuxerContext, priv_data) + impl->priv_data_size))) {
        fprintf(stderr, "Failed to allocate memory\n");
        return DAV1D_ERR(ENOMEM);
    }
    c->impl = impl;
    c->data = (DemuxerPriv *) c->priv_data;

    if ((res = impl->open(c->data, fname, fps, num_frames, timebase)) < 0) {
        if (!strcmp(filename, "-")) unlink(fname);
        free(c);
        return res;
    }
    *c_out = c;

    if (!strcmp(filename, "-")) unlink(fname);
    return 0;
}

int input_read(DemuxerContext *const ctx, Dav1dData *const data) {
    return ctx->impl->read(ctx->data, data);
}

int input_seek(DemuxerContext *const ctx, const uint64_t pts) {
    return ctx->impl->seek ? ctx->impl->seek(ctx->data, pts) : -1;
}

void input_close(DemuxerContext *const ctx) {
    ctx->impl->close(ctx->data);
    free(ctx);
}
