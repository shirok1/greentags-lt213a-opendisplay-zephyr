/*
 * OpenDisplay streaming zlib inflater, based on uzlib/tinf.
 * LT213A modifications: four-bit code lengths and canonical rank lookup.
 * Only the temporary code-length alphabet needs a symbol translation table.
 */

#include <string.h>
#include <stdlib.h>
#include "uzlib.h"

#define TINF_ARRAY_SIZE(arr) (sizeof(arr) / sizeof(*(arr)))

#ifndef OPENDISPLAY_ZLIB_USE_HEAP_WINDOW
#define OPENDISPLAY_ZLIB_USE_HEAP_WINDOW 0
#endif

#if OPENDISPLAY_ZLIB_USE_HEAP_WINDOW != 0 && OPENDISPLAY_ZLIB_USE_HEAP_WINDOW != 1
#error "OPENDISPLAY_ZLIB_USE_HEAP_WINDOW must be 0 or 1"
#endif

typedef struct {
    unsigned short table[16];
    uint8_t trans[19];
} TINF_CODELEN_TREE;

static const unsigned char length_bits[30] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5
};

static const unsigned short length_base[30] = {
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258
};

static const unsigned char dist_bits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,
    3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13,
    17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577
};

static const unsigned char clcidx[] = {
    16, 17, 18, 0, 8, 7, 9, 6,
    10, 5, 11, 4, 12, 3, 13, 2,
    14, 1, 15
};

typedef enum {
    ST_ZLIB_CMF,
    ST_ZLIB_FLG,
    ST_BLOCK_FINAL,
    ST_BLOCK_TYPE,
    ST_DYNAMIC_TREES,
    ST_STORED_LEN,
    ST_STORED_DATA,
    ST_BLOCK_DATA,
    ST_TRAILER,
    ST_DONE,
    ST_ERROR,
} inflate_stage_t;

typedef enum {
    BLK_SYMBOL,
    BLK_LEN_EXTRA,
    BLK_DIST_SYMBOL,
    BLK_DIST_EXTRA,
    BLK_MATCH_COPY,
} block_stage_t;

typedef enum {
    DYN_HLIT,
    DYN_HDIST,
    DYN_HCLEN,
    DYN_CLEAR_CLEN,
    DYN_READ_CLEN,
    DYN_BUILD_CLTREE,
    DYN_READ_LENGTHS,
    DYN_REPEAT_BITS,
    DYN_BUILD_TREES,
} dynamic_stage_t;

typedef struct {
    const uint8_t *input;
    const char *error;
    size_t input_remaining;
    unsigned int read_bits_base;
    unsigned int read_bits_value;
    int sym_cur; /* A malformed 16-bit path can reach 65535 before rejection. */
    unsigned int match_len;
    unsigned int match_offset;
    uint32_t expected_output;
    uint32_t output_count;
    uint32_t adler;

    /* Header decoding and data decoding are mutually exclusive. Retain the
     * packed lengths for canonical symbol lookup instead of translation arrays. */
    union {
        struct {
            unsigned short literal[16];
            unsigned short distance[16];
        } data;
        struct {
            TINF_CODELEN_TREE codes;
            uint16_t hlimit, dyn_num;
            uint8_t hclen, dyn_i, dyn_fill_value;
            uint8_t dyn_repeat_len, dyn_repeat_bits, dyn_repeat_base;
            dynamic_stage_t stage;
        } header;
        struct { uint16_t len, invlen; uint8_t index; } stored;
        struct { uint32_t value; uint8_t index; } trailer;
    } tree;
    unsigned char lengths[(288 + 32) / 2];
#if OPENDISPLAY_ZLIB_USE_HEAP_WINDOW
    uint8_t *window;
#else
    uint8_t window[OPENDISPLAY_ZLIB_WINDOW_SIZE];
#endif

    /* DEFLATE has at most 320 lengths and a 32768-byte history window.
     * Group bounded fields by width; no packed/unaligned member accesses. */
    int16_t sym_sum;
    uint16_t hlit, window_pos;
    inflate_stage_t stage;
    block_stage_t block_stage;
    bool input_final, initialized, read_bits_active, sym_active;
    uint8_t bit_tag, bit_count, read_bits_num, read_bits_pos;
    uint8_t cmf, bfinal, btype, sym_len;
    uint8_t hdist;
    uint8_t length_sym, dist_sym;
} od_zlib_stream_state_t;

static od_zlib_stream_state_t s;
#if OPENDISPLAY_ZLIB_USE_HEAP_WINDOW
static uint8_t *s_window;
#endif

static void set_error(const char *error) {
    s.stage = ST_ERROR;
    s.error = error;
}

static unsigned get_length(unsigned i) { return (s.lengths[i / 2] >> ((i & 1) * 4)) & 15; }
static void set_length(unsigned i, unsigned value) {
    unsigned shift = (i & 1) * 4;
    s.lengths[i / 2] = (s.lengths[i / 2] & ~(15u << shift)) | (value << shift);
}

static bool build_tree(unsigned short *table, uint8_t *trans, unsigned int trans_size, unsigned base, unsigned int num) {
    unsigned short offs[16];
    unsigned int i, sum;

    for (i = 0; i < 16; ++i) table[i] = 0;
    for (i = 0; i < num; ++i) {
        if (get_length(base + i) >= TINF_ARRAY_SIZE(offs)) {
            set_error("invalid huffman code length");
            return false;
        }
        table[get_length(base + i)]++;
    }
    table[0] = 0;

    for (sum = 0, i = 0; i < 16; ++i) {
        offs[i] = sum;
        sum += table[i];
    }
    if (sum > trans_size) {
        set_error("huffman tree exceeds symbol storage");
        return false;
    }

    for (i = 0; i < num; ++i) {
        if (trans && get_length(base + i)) trans[offs[get_length(base + i)]++] = (uint8_t)i;
    }
    return true;
}

static void build_fixed_trees(void) {
    unsigned i;
    memset(&s.tree.data, 0, sizeof(s.tree.data));
    s.hlit = 288;
    s.hdist = 32;
    s.tree.data.literal[7] = 24;
    s.tree.data.literal[8] = 152;
    s.tree.data.literal[9] = 112;
    s.tree.data.distance[5] = 32;
    for (i = 0; i < 144; ++i) set_length(i, 8);
    for (; i < 256; ++i) set_length(i, 9);
    for (; i < 280; ++i) set_length(i, 7);
    for (; i < 288; ++i) set_length(i, 8);
    for (; i < 320; ++i) set_length(i, 5);
}

static int read_byte(uint8_t *value) {
    if (s.input_remaining > 0) {
        *value = *s.input++;
        s.input_remaining--;
        return 1;
    }
    if (!s.input_final) return 0;
    set_error("truncated zlib stream");
    return -1;
}

static int read_bit(unsigned int *bit) {
    uint8_t byte;
    if (s.bit_count == 0) {
        int rc = read_byte(&byte);
        if (rc <= 0) return rc;
        s.bit_tag = byte;
        s.bit_count = 8;
    }
    *bit = s.bit_tag & 1u;
    s.bit_tag >>= 1u;
    s.bit_count--;
    return 1;
}

static int read_bits(unsigned int num, unsigned int base, unsigned int *value) {
    if (!s.read_bits_active) {
        s.read_bits_active = true;
        s.read_bits_num = (uint8_t)num;
        s.read_bits_pos = 0;
        s.read_bits_base = base;
        s.read_bits_value = 0;
    }

    while (s.read_bits_pos < s.read_bits_num) {
        unsigned int bit;
        int rc = read_bit(&bit);
        if (rc <= 0) return rc;
        if (bit) s.read_bits_value += 1u << s.read_bits_pos;
        s.read_bits_pos++;
    }

    *value = s.read_bits_value + s.read_bits_base;
    s.read_bits_active = false;
    return 1;
}

static int decode_symbol(const unsigned short *table, const uint8_t *trans, unsigned int trans_size, unsigned base, int *symbol) {
    if (!s.sym_active) {
        s.sym_active = true;
        s.sym_sum = 0;
        s.sym_cur = 0;
        s.sym_len = 0;
    }

    do {
        unsigned int bit;
        int rc = read_bit(&bit);
        if (rc <= 0) return rc;

        s.sym_cur = 2 * s.sym_cur + (int)bit;
        if (++s.sym_len == 16) {
            set_error("invalid huffman code");
            return -1;
        }

        s.sym_sum += table[s.sym_len];
        s.sym_cur -= table[s.sym_len];
    } while (s.sym_cur >= 0);

    s.sym_sum += s.sym_cur;
    if (s.sym_sum < 0 || s.sym_sum >= (int)trans_size) {
        set_error("invalid huffman symbol");
        return -1;
    }
    if (trans) {
        *symbol = trans[s.sym_sum];
    } else {
        /* Canonical codes order equal-length symbols by numeric value.
         * sym_cur is the negative offset from the end of this length group. */
        int rank = s.sym_cur + table[s.sym_len];
        int reverse_rank = table[s.sym_len] - 1 - rank;
        unsigned left = 0, right = trans_size;
        *symbol = -1;
        /* Sparse alphabets often place literals at either end (0x00/0xff).
         * Search both ends without another RAM translation table. */
        while (left < right) {
            if (get_length(base + left) == (unsigned)s.sym_len && rank-- == 0) {
                *symbol = (int)left;
                break;
            }
            if (++left == right) break;
            --right;
            if (get_length(base + right) == (unsigned)s.sym_len && reverse_rank-- == 0) {
                *symbol = (int)right;
                break;
            }
        }
        if (*symbol < 0) {
            set_error("invalid huffman symbol");
            return -1;
        }
    }
    s.sym_active = false;
    return 1;
}

static void reset_code_readers(void) {
    s.read_bits_active = false;
    s.sym_active = false;
}

static void adler_update_byte(uint8_t byte) {
    uint32_t s1 = s.adler & 0xffffu;
    uint32_t s2 = s.adler >> 16;
    s1 += byte;
    if (s1 >= 65521u) s1 -= 65521u;
    s2 += s1;
    /* Both addends are below 65521, so one subtraction replaces division. */
    if (s2 >= 65521u) s2 -= 65521u;
    s.adler = (s2 << 16) | s1;
}

static bool put_output_byte(uint8_t byte, uint8_t *output, size_t capacity, size_t *produced) {
    if (*produced >= capacity) return false;
    if (s.output_count >= s.expected_output) {
        set_error("decompressed output exceeds expected size");
        return false;
    }

    output[(*produced)++] = byte;
    s.window[s.window_pos] = byte;
    s.window_pos = (s.window_pos + 1u) & (OPENDISPLAY_ZLIB_WINDOW_SIZE - 1u);
    s.output_count++;
    adler_update_byte(byte);
    return true;
}

static int process_dynamic_trees(void) {
    int sym;
    unsigned int value;

    for (;;) {
        switch (s.tree.header.stage) {
        case DYN_HLIT:
            if (read_bits(5, 257, &value) <= 0) return s.stage == ST_ERROR ? -1 : 0;
            s.hlit = value;
            s.tree.header.stage = DYN_HDIST;
            break;
        case DYN_HDIST:
            if (read_bits(5, 1, &value) <= 0) return s.stage == ST_ERROR ? -1 : 0;
            s.hdist = value;
            s.tree.header.stage = DYN_HCLEN;
            break;
        case DYN_HCLEN:
            if (read_bits(4, 4, &value) <= 0) return s.stage == ST_ERROR ? -1 : 0;
            s.tree.header.hclen = value;
            if (s.hlit > 286 || s.hdist > 32 || s.hlit + s.hdist > (2 * TINF_ARRAY_SIZE(s.lengths))) {
                set_error("invalid dynamic tree sizes");
                return -1;
            }
            s.tree.header.dyn_i = 0;
            s.tree.header.stage = DYN_CLEAR_CLEN;
            break;
        case DYN_CLEAR_CLEN:
            while (s.tree.header.dyn_i < 19) set_length(s.tree.header.dyn_i++, 0);
            s.tree.header.dyn_i = 0;
            s.tree.header.stage = DYN_READ_CLEN;
            break;
        case DYN_READ_CLEN:
            while (s.tree.header.dyn_i < s.tree.header.hclen) {
                if (read_bits(3, 0, &value) <= 0) return s.stage == ST_ERROR ? -1 : 0;
                set_length(clcidx[s.tree.header.dyn_i++], value);
            }
            s.tree.header.stage = DYN_BUILD_CLTREE;
            break;
        case DYN_BUILD_CLTREE:
            if (!build_tree(s.tree.header.codes.table, s.tree.header.codes.trans, TINF_ARRAY_SIZE(s.tree.header.codes.trans), 0, 19)) return -1;
            s.tree.header.hlimit = s.hlit + s.hdist;
            s.tree.header.dyn_num = 0;
            s.tree.header.stage = DYN_READ_LENGTHS;
            reset_code_readers();
            break;
        case DYN_READ_LENGTHS:
            while (s.tree.header.dyn_num < s.tree.header.hlimit) {
                int rc = decode_symbol(s.tree.header.codes.table, s.tree.header.codes.trans, TINF_ARRAY_SIZE(s.tree.header.codes.trans), 0, &sym);
                if (rc <= 0) return s.stage == ST_ERROR ? -1 : 0;
                if (sym < 16) {
                    set_length(s.tree.header.dyn_num++, sym);
                    continue;
                }
                if (sym == 16) {
                    if (s.tree.header.dyn_num == 0) {
                        set_error("invalid dynamic repeat");
                        return -1;
                    }
                    s.tree.header.dyn_fill_value = get_length(s.tree.header.dyn_num - 1);
                    s.tree.header.dyn_repeat_bits = 2;
                    s.tree.header.dyn_repeat_base = 3;
                } else if (sym == 17) {
                    s.tree.header.dyn_fill_value = 0;
                    s.tree.header.dyn_repeat_bits = 3;
                    s.tree.header.dyn_repeat_base = 3;
                } else if (sym == 18) {
                    s.tree.header.dyn_fill_value = 0;
                    s.tree.header.dyn_repeat_bits = 7;
                    s.tree.header.dyn_repeat_base = 11;
                } else {
                    set_error("invalid dynamic code length symbol");
                    return -1;
                }
                s.tree.header.stage = DYN_REPEAT_BITS;
                break;
            }
            if (s.tree.header.stage != DYN_READ_LENGTHS) break;
            s.tree.header.stage = DYN_BUILD_TREES;
            break;
        case DYN_REPEAT_BITS:
            if (read_bits(s.tree.header.dyn_repeat_bits, s.tree.header.dyn_repeat_base, &value) <= 0) {
                return s.stage == ST_ERROR ? -1 : 0;
            }
            s.tree.header.dyn_repeat_len = value;
            if (s.tree.header.dyn_num + s.tree.header.dyn_repeat_len > s.tree.header.hlimit) {
                set_error("dynamic repeat exceeds tree size");
                return -1;
            }
            while (s.tree.header.dyn_repeat_len > 0) {
                set_length(s.tree.header.dyn_num++, s.tree.header.dyn_fill_value);
                s.tree.header.dyn_repeat_len--;
            }
            s.tree.header.stage = DYN_READ_LENGTHS;
            break;
        case DYN_BUILD_TREES:
            if (get_length(256) == 0) {
                set_error("dynamic tree missing end-of-block");
                return -1;
            }
            if (!build_tree(s.tree.data.literal, NULL, s.hlit, 0, s.hlit)) return -1;
            if (!build_tree(s.tree.data.distance, NULL, s.hdist, s.hlit, s.hdist)) return -1;
            reset_code_readers();
            return 1;
        }
    }
}

static int process_stored_header(void) {
    uint8_t byte;
    while (s.tree.stored.index < 4) {
        int rc = read_byte(&byte);
        if (rc <= 0) return rc;
        if (s.tree.stored.index == 0) s.tree.stored.len = byte;
        else if (s.tree.stored.index == 1) s.tree.stored.len |= (uint16_t)byte << 8;
        else if (s.tree.stored.index == 2) s.tree.stored.invlen = byte;
        else s.tree.stored.invlen |= (uint16_t)byte << 8;
        s.tree.stored.index++;
    }
    if (s.tree.stored.len != (uint16_t)(~s.tree.stored.invlen)) {
        set_error("invalid stored block length");
        return -1;
    }
    s.tree.stored.index = 0;
    return 1;
}

static int process_stored_data(uint8_t *output, size_t capacity, size_t *produced) {
    while (s.tree.stored.len > 0) {
        uint8_t byte;
        int rc;
        if (*produced >= capacity) return 2;
        rc = read_byte(&byte);
        if (rc <= 0) return rc;
        if (!put_output_byte(byte, output, capacity, produced)) return s.stage == ST_ERROR ? -1 : 2;
        s.tree.stored.len--;
    }
    return 1;
}

static int process_block_data(uint8_t *output, size_t capacity, size_t *produced) {
    int sym;

    for (;;) {
        if (*produced >= capacity) return 2;
        switch (s.block_stage) {
        case BLK_SYMBOL: {
            int rc = decode_symbol(s.tree.data.literal, NULL, s.hlit, 0, &sym);
            if (rc <= 0) return rc;
            if (sym < 256) {
                if (!put_output_byte((uint8_t)sym, output, capacity, produced)) return s.stage == ST_ERROR ? -1 : 2;
                return 1;
            }
            if (sym == 256) {
                s.block_stage = BLK_SYMBOL;
                return 3;
            }
            sym -= 257;
            if (sym < 0 || sym >= 29) {
                set_error("invalid length symbol");
                return -1;
            }
            s.length_sym = sym;
            s.block_stage = BLK_LEN_EXTRA;
            break;
        }
        case BLK_LEN_EXTRA:
            if (read_bits(length_bits[s.length_sym], length_base[s.length_sym], &s.match_len) <= 0) {
                return s.stage == ST_ERROR ? -1 : 0;
            }
            s.block_stage = BLK_DIST_SYMBOL;
            break;
        case BLK_DIST_SYMBOL: {
            int rc = decode_symbol(s.tree.data.distance, NULL, s.hdist, s.hlit, &sym);
            if (rc <= 0) return rc;
            if (sym < 0 || sym >= 30) {
                set_error("invalid distance symbol");
                return -1;
            }
            s.dist_sym = sym;
            s.block_stage = BLK_DIST_EXTRA;
            break;
        }
        case BLK_DIST_EXTRA:
            if (read_bits(dist_bits[s.dist_sym], dist_base[s.dist_sym], &s.match_offset) <= 0) {
                return s.stage == ST_ERROR ? -1 : 0;
            }
            if (s.match_offset == 0 || s.match_offset > OPENDISPLAY_ZLIB_WINDOW_SIZE || s.match_offset > s.output_count) {
                set_error("invalid back-reference distance");
                return -1;
            }
            s.block_stage = BLK_MATCH_COPY;
            break;
        case BLK_MATCH_COPY:
            while (s.match_len > 0) {
                uint8_t byte;
                unsigned int source;
                if (*produced >= capacity) return 2;
                source = (s.window_pos + OPENDISPLAY_ZLIB_WINDOW_SIZE - s.match_offset) & (OPENDISPLAY_ZLIB_WINDOW_SIZE - 1u);
                byte = s.window[source];
                if (!put_output_byte(byte, output, capacity, produced)) return s.stage == ST_ERROR ? -1 : 2;
                s.match_len--;
            }
            s.block_stage = BLK_SYMBOL;
            break;
        }
    }
}

/* Block-specific scratch is shared. Initialize the trailer on transition,
 * including after stored blocks and when the last checksum bytes arrive later. */
static void end_block(void) {
    if (s.bfinal) {
        s.tree.trailer.value = 0;
        s.tree.trailer.index = 0;
        s.stage = ST_TRAILER;
    } else {
        s.stage = ST_BLOCK_FINAL;
    }
}

static int process_trailer(void) {
    uint8_t byte;
    while (s.tree.trailer.index < 4) {
        int rc = read_byte(&byte);
        if (rc <= 0) return rc;
        s.tree.trailer.value = (s.tree.trailer.value << 8) | byte;
        s.tree.trailer.index++;
    }
    if (s.tree.trailer.value != s.adler) {
        set_error("zlib adler32 mismatch");
        return -1;
    }
    if (s.output_count != s.expected_output) {
        set_error("decompressed output size mismatch");
        return -1;
    }
    if (s.input_remaining > 0) {
        set_error("input after end of zlib stream");
        return -1;
    }
    s.stage = ST_DONE;
    return 1;
}

void od_zlib_stream_reset(uint32_t expected_output_size) {
#if OPENDISPLAY_ZLIB_USE_HEAP_WINDOW
    if (s_window == NULL) {
        s_window = (uint8_t *)malloc(OPENDISPLAY_ZLIB_WINDOW_SIZE);
    }
#endif
    memset(&s, 0, sizeof(s));
#if OPENDISPLAY_ZLIB_USE_HEAP_WINDOW
    if (s_window == NULL) {
        s.initialized = true;
        set_error("zlib history window allocation failed");
        return;
    }
    s.window = s_window;
#endif
    s.stage = ST_ZLIB_CMF;
    s.block_stage = BLK_SYMBOL;
    s.tree.header.stage = DYN_HLIT;
    s.expected_output = expected_output_size;
    s.adler = 1;
    s.initialized = true;
}

od_zlib_status_t od_zlib_stream_push(const uint8_t *input, size_t len, bool final) {
    if (!s.initialized) {
        set_error("zlib stream not initialized");
        return OD_ZLIB_STATUS_ERROR;
    }
    if (len > 0 && input == NULL) {
        set_error("zlib stream input is null");
        return OD_ZLIB_STATUS_ERROR;
    }
    if (s.input_remaining > 0) {
        set_error("previous input not fully consumed");
        return OD_ZLIB_STATUS_ERROR;
    }
    s.input = input;
    s.input_remaining = len;
    s.input_final = final;
    if (s.stage == ST_DONE) {
        if (len != 0) {
            set_error("input after end of zlib stream");
            return OD_ZLIB_STATUS_ERROR;
        }
        return OD_ZLIB_STATUS_DONE;
    }
    return OD_ZLIB_STATUS_NEEDS_INPUT;
}

od_zlib_status_t od_zlib_stream_poll(uint8_t *output, size_t capacity, size_t *produced) {
    *produced = 0;
    if (!s.initialized) {
        set_error("zlib stream not initialized");
        return OD_ZLIB_STATUS_ERROR;
    }
    if (s.stage == ST_ERROR) return OD_ZLIB_STATUS_ERROR;
    if (s.stage == ST_DONE) return OD_ZLIB_STATUS_DONE;

    for (;;) {
        int rc;
        unsigned int value;
        uint8_t byte;

        if (*produced >= capacity) return OD_ZLIB_STATUS_OUTPUT_READY;

        switch (s.stage) {
        case ST_ZLIB_CMF:
            rc = read_byte(&s.cmf);
            if (rc <= 0) return s.stage == ST_ERROR ? OD_ZLIB_STATUS_ERROR : OD_ZLIB_STATUS_NEEDS_INPUT;
            s.stage = ST_ZLIB_FLG;
            break;
        case ST_ZLIB_FLG:
            rc = read_byte(&byte);
            if (rc <= 0) return s.stage == ST_ERROR ? OD_ZLIB_STATUS_ERROR : OD_ZLIB_STATUS_NEEDS_INPUT;
            if (((256u * s.cmf + byte) % 31u) != 0 || (s.cmf & 0x0fu) != 8u || (byte & 0x20u) != 0) {
                set_error("invalid zlib header");
                return OD_ZLIB_STATUS_ERROR;
            }
            if (((s.cmf >> 4) + 8u) > OPENDISPLAY_ZLIB_WINDOW_BITS) {
                set_error("zlib stream window exceeds firmware limit");
                return OD_ZLIB_STATUS_ERROR;
            }
            s.stage = ST_BLOCK_FINAL;
            break;
        case ST_BLOCK_FINAL:
            rc = read_bit(&value);
            if (rc <= 0) return s.stage == ST_ERROR ? OD_ZLIB_STATUS_ERROR : OD_ZLIB_STATUS_NEEDS_INPUT;
            s.bfinal = (uint8_t)value;
            s.stage = ST_BLOCK_TYPE;
            break;
        case ST_BLOCK_TYPE:
            rc = read_bits(2, 0, &value);
            if (rc <= 0) return s.stage == ST_ERROR ? OD_ZLIB_STATUS_ERROR : OD_ZLIB_STATUS_NEEDS_INPUT;
            s.btype = (uint8_t)value;
            if (s.btype == 0) {
                s.bit_count = 0;
                s.tree.stored.index = 0;
                s.tree.stored.len = 0;
                s.tree.stored.invlen = 0;
                s.stage = ST_STORED_LEN;
            } else if (s.btype == 1) {
                build_fixed_trees();
                s.block_stage = BLK_SYMBOL;
                reset_code_readers();
                s.stage = ST_BLOCK_DATA;
            } else if (s.btype == 2) {
                s.tree.header.stage = DYN_HLIT;
                reset_code_readers();
                s.stage = ST_DYNAMIC_TREES;
            } else {
                set_error("invalid deflate block type");
                return OD_ZLIB_STATUS_ERROR;
            }
            break;
        case ST_DYNAMIC_TREES:
            rc = process_dynamic_trees();
            if (rc < 0) return OD_ZLIB_STATUS_ERROR;
            if (rc == 0) return OD_ZLIB_STATUS_NEEDS_INPUT;
            s.block_stage = BLK_SYMBOL;
            s.stage = ST_BLOCK_DATA;
            break;
        case ST_STORED_LEN:
            rc = process_stored_header();
            if (rc < 0) return OD_ZLIB_STATUS_ERROR;
            if (rc == 0) return OD_ZLIB_STATUS_NEEDS_INPUT;
            s.stage = ST_STORED_DATA;
            break;
        case ST_STORED_DATA:
            rc = process_stored_data(output, capacity, produced);
            if (rc < 0) return OD_ZLIB_STATUS_ERROR;
            if (rc == 0) return *produced ? OD_ZLIB_STATUS_OUTPUT_READY : OD_ZLIB_STATUS_NEEDS_INPUT;
            if (rc == 2) return OD_ZLIB_STATUS_OUTPUT_READY;
            end_block();
            break;
        case ST_BLOCK_DATA:
            rc = process_block_data(output, capacity, produced);
            if (rc < 0) return OD_ZLIB_STATUS_ERROR;
            if (rc == 0) return *produced ? OD_ZLIB_STATUS_OUTPUT_READY : OD_ZLIB_STATUS_NEEDS_INPUT;
            if (rc == 2) return OD_ZLIB_STATUS_OUTPUT_READY;
            if (rc == 3) end_block();
            break;
        case ST_TRAILER:
            rc = process_trailer();
            if (rc < 0) return OD_ZLIB_STATUS_ERROR;
            if (rc == 0) return *produced ? OD_ZLIB_STATUS_OUTPUT_READY : OD_ZLIB_STATUS_NEEDS_INPUT;
            return OD_ZLIB_STATUS_DONE;
        case ST_DONE:
            return OD_ZLIB_STATUS_DONE;
        case ST_ERROR:
            return OD_ZLIB_STATUS_ERROR;
        }
    }
}

const char *od_zlib_stream_error(void) {
    return s.error ? s.error : "";
}

uint32_t od_zlib_stream_output_count(void) {
    return s.output_count;
}
