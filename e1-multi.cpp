#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <typeinfo>
#include <stdio.h>

#include "timer.h"
#include "mymacros.h"
#include "sse.h"

typedef unsigned char byte;

static const size_t NUM_TIMESLOTS = 32;
static const size_t DST_SIZE = 64;
static const size_t SRC_SIZE = NUM_TIMESLOTS * DST_SIZE;
static const size_t MIN_COUNT = 1;
static const size_t MAX_COUNT = 1024 * 1024;
static const unsigned ITERATIONS = 1024 * 1024;

using namespace std;

class Demux
{
public:
    virtual void demux (const byte * src, size_t src_length, byte ** dst) const = 0;
};

class Src_First_1 : public Demux
{
public:
    void demux (const byte * src, size_t src_length, byte ** dst) const
    {
        assert (src_length % NUM_TIMESLOTS == 0);

        size_t src_pos = 0;
        size_t dst_pos = 0;
        while (src_pos < src_length) {
            for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; ++ dst_num) {
                dst [dst_num][dst_pos] = src [src_pos ++];
            }
            ++ dst_pos;
        }
    }
};

class Dst_First_3a : public Demux
{
public:
    void demux (const byte * src, size_t src_length, byte ** dst) const
    {
        assert (src_length == NUM_TIMESLOTS * DST_SIZE);

        for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; ++ dst_num) {
            byte * d = dst [dst_num];
            for (size_t dst_pos = 0; dst_pos < DST_SIZE; ++ dst_pos) {
                d [dst_pos] = src [dst_pos * NUM_TIMESLOTS + dst_num];
            }
        }
    }
};

inline uint32_t make_32(byte b0, byte b1, byte b2, byte b3)
{
    return ((uint32_t)b0 << 0)
        | ((uint32_t)b1 << 8)
        | ((uint32_t)b2 << 16)
        | ((uint32_t)b3 << 24);
}

inline uint64_t make_64(byte b0, byte b1, byte b2, byte b3, byte b4, byte b5, byte b6, byte b7)
{
    return (uint64_t)make_32(b0, b1, b2, b3)
        | ((uint64_t)b4 << 32)
        | ((uint64_t)b5 << 40)
        | ((uint64_t)b6 << 48)
        | ((uint64_t)b7 << 56);
}

class Write8 : public Demux
{
public:
    void demux(const byte * src, size_t src_length, byte ** dst) const
    {
        assert(src_length == NUM_TIMESLOTS * DST_SIZE);
        assert(DST_SIZE % 8 == 0);

        for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; ++dst_num) {
            byte * d = dst[dst_num];
            for (size_t dst_pos = 0; dst_pos < DST_SIZE; dst_pos += 8) {
                byte b0 = src[(dst_pos + 0) * NUM_TIMESLOTS + dst_num];
                byte b1 = src[(dst_pos + 1) * NUM_TIMESLOTS + dst_num];
                byte b2 = src[(dst_pos + 2) * NUM_TIMESLOTS + dst_num];
                byte b3 = src[(dst_pos + 3) * NUM_TIMESLOTS + dst_num];
                byte b4 = src[(dst_pos + 4) * NUM_TIMESLOTS + dst_num];
                byte b5 = src[(dst_pos + 5) * NUM_TIMESLOTS + dst_num];
                byte b6 = src[(dst_pos + 6) * NUM_TIMESLOTS + dst_num];
                byte b7 = src[(dst_pos + 7) * NUM_TIMESLOTS + dst_num];
                *(uint64_t*)& d[dst_pos] = make_64(b0, b1, b2, b3, b4, b5, b6, b7);
            }
        }
    }
};

class Read8_Write16_SSE_Unroll : public Demux
{
public:
    void demux (const byte * src, size_t src_length, byte ** dst) const
    {
        assert (src_length == NUM_TIMESLOTS * DST_SIZE);
        assert (DST_SIZE == 64);
        assert (NUM_TIMESLOTS % 8 == 0);

        for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; dst_num += 8) {
            byte * d0 = dst [dst_num + 0];
            byte * d1 = dst [dst_num + 1];
            byte * d2 = dst [dst_num + 2];
            byte * d3 = dst [dst_num + 3];
            byte * d4 = dst [dst_num + 4];
            byte * d5 = dst [dst_num + 5];
            byte * d6 = dst [dst_num + 6];
            byte * d7 = dst [dst_num + 7];

#define LOAD32(m0, m1, dst_pos) do {\
                    __m64 w0 = * (__m64 *) &src [(dst_pos + 0) * NUM_TIMESLOTS + dst_num];\
                    __m64 w1 = * (__m64 *) &src [(dst_pos + 1) * NUM_TIMESLOTS + dst_num];\
                    __m64 w2 = * (__m64 *) &src [(dst_pos + 2) * NUM_TIMESLOTS + dst_num];\
                    __m64 w3 = * (__m64 *) &src [(dst_pos + 3) * NUM_TIMESLOTS + dst_num];\
                    __m128i x0 = _mm_setr_epi64 (w0, w1);\
                    __m128i x1 = _mm_setr_epi64 (w2, w3);\
                    m0 = _128i_shuffle (x0, x1, 0, 2, 0, 2);\
                    m1 = _128i_shuffle (x0, x1, 1, 3, 1, 3);\
                    m0 = transpose_4x4 (m0);\
                    m1 = transpose_4x4 (m1);\
                } while (0)

#define MOVE128(dst_pos) do {\
                __m128i a0, a1, a2, a3, b0, b1, b2, b3;\
                LOAD32 (a0, b0, dst_pos);\
                LOAD32 (a1, b1, dst_pos + 4);\
                LOAD32 (a2, b2, dst_pos + 8);\
                LOAD32 (a3, b3, dst_pos + 12);\
                transpose_4x4_dwords (a0, a1, a2, a3);\
                _128i_store (&d0 [dst_pos], a0);\
                _128i_store (&d1 [dst_pos], a1);\
                _128i_store (&d2 [dst_pos], a2);\
                _128i_store (&d3 [dst_pos], a3);\
                transpose_4x4_dwords (b0, b1, b2, b3);\
                _128i_store (&d4 [dst_pos], b0);\
                _128i_store (&d5 [dst_pos], b1);\
                _128i_store (&d6 [dst_pos], b2);\
                _128i_store (&d7 [dst_pos], b3);\
            } while (0)

            MOVE128 (0);
            MOVE128 (16);
            MOVE128 (32);
            MOVE128 (48);
#undef LOAD32
#undef MOVE128
        }
    }
};

class Read8_Write32_AVX_Unroll : public Demux
{
public:
    void demux (const byte * src, size_t src_length, byte ** dst) const
    {
        assert (src_length == NUM_TIMESLOTS * DST_SIZE);
        assert (DST_SIZE == 64);
        assert (NUM_TIMESLOTS % 8 == 0);

        for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; dst_num += 8) {
            byte * d0 = dst [dst_num + 0];
            byte * d1 = dst [dst_num + 1];
            byte * d2 = dst [dst_num + 2];
            byte * d3 = dst [dst_num + 3];
            byte * d4 = dst [dst_num + 4];
            byte * d5 = dst [dst_num + 5];
            byte * d6 = dst [dst_num + 6];
            byte * d7 = dst [dst_num + 7];

#define LOAD32(m0, m1, dst_pos) do {\
                    __m64 w0 = * (__m64 *) &src [(dst_pos + 0) * NUM_TIMESLOTS + dst_num];\
                    __m64 w1 = * (__m64 *) &src [(dst_pos + 1) * NUM_TIMESLOTS + dst_num];\
                    __m64 w2 = * (__m64 *) &src [(dst_pos + 2) * NUM_TIMESLOTS + dst_num];\
                    __m64 w3 = * (__m64 *) &src [(dst_pos + 3) * NUM_TIMESLOTS + dst_num];\
                    __m128i x0 = _mm_setr_epi64 (w0, w1);\
                    __m128i x1 = _mm_setr_epi64 (w2, w3);\
                    m0 = _128i_shuffle (x0, x1, 0, 2, 0, 2);\
                    m1 = _128i_shuffle (x0, x1, 1, 3, 1, 3);\
                    m0 = transpose_4x4 (m0);\
                    m1 = transpose_4x4 (m1);\
                } while (0)

#define MOVE256(dst_pos) do {\
                __m128i a0, a1, a2, a3, b0, b1, b2, b3;\
                LOAD32 (a0, b0, dst_pos);\
                LOAD32 (a1, b1, dst_pos + 4);\
                LOAD32 (a2, b2, dst_pos + 8);\
                LOAD32 (a3, b3, dst_pos + 12);\
\
                __m128i c0, c1, c2, c3, e0, e1, e2, e3;\
                LOAD32 (c0, e0, dst_pos + 16);\
                LOAD32 (c1, e1, dst_pos + 20);\
                LOAD32 (c2, e2, dst_pos + 24);\
                LOAD32 (c3, e3, dst_pos + 28);\
\
                __m256i w0 = _256i_combine_lo_hi (a0, c0);\
                __m256i w1 = _256i_combine_lo_hi (a1, c1);\
                __m256i w2 = _256i_combine_lo_hi (a2, c2);\
                __m256i w3 = _256i_combine_lo_hi (a3, c3);\
                __m256i w4 = _256i_combine_lo_hi (b0, e0);\
                __m256i w5 = _256i_combine_lo_hi (b1, e1);\
                __m256i w6 = _256i_combine_lo_hi (b2, e2);\
                __m256i w7 = _256i_combine_lo_hi (b3, e3);\
\
                transpose_avx_4x4_dwords (w0, w1, w2, w3);\
                _256i_store (&d0 [dst_pos], w0);\
                _256i_store (&d1 [dst_pos], w1);\
                _256i_store (&d2 [dst_pos], w2);\
                _256i_store (&d3 [dst_pos], w3);\
\
                transpose_avx_4x4_dwords (w4, w5, w6, w7);\
                _256i_store (&d4 [dst_pos], w4);\
                _256i_store (&d5 [dst_pos], w5);\
                _256i_store (&d6 [dst_pos], w6);\
                _256i_store (&d7 [dst_pos], w7);\
            } while (0)

            MOVE256 (0);
            MOVE256 (32);
#undef LOAD32
#undef MOVE256
        }
    }
};

class Copy_AVX: public Demux
{
public:
    void demux (const byte * src, size_t src_length, byte ** dst) const
    {
        assert (src_length == NUM_TIMESLOTS * DST_SIZE);
        assert (DST_SIZE % 32 == 0);

        for (size_t dst_num = 0; dst_num < NUM_TIMESLOTS; dst_num ++) {
            byte * d = dst [dst_num];
            for (size_t dst_pos = 0; dst_pos < DST_SIZE; dst_pos += 32) {
                _256i_store (d + dst_pos,
                             _mm256_load_si256 ((__m256i const *) (src + dst_num * DST_SIZE + dst_pos)));
            }
        }
    }
};

byte * generate (size_t count)
{
    byte * buf = (byte*)_mm_malloc(SRC_SIZE * count, 32);
    memset(buf, (byte) 0xEE, SRC_SIZE * count);
    return buf;
}

byte ** allocate_dst(size_t count)
{
    byte * buf = (byte*)_mm_malloc(SRC_SIZE * count, 32);
    memset (buf, 0xDD, SRC_SIZE * count);
    byte ** result = new byte *[NUM_TIMESLOTS * count];
    for (size_t i = 0; i < NUM_TIMESLOTS * count; i++) {
        result[i] = buf + i * DST_SIZE;
    }
    return result;
}

byte * src;
byte ** dst;

void measure_base (const Demux & demux)
{
    printf("      %-30s:", typeid (demux).name());
    fflush(stdout);
    unsigned iterations = ITERATIONS;
    int64_t tfirst;

    for (unsigned count = MIN_COUNT; count <= MAX_COUNT; count *= 2) {
        uint64_t t0 = currentTimeMillis();
        for (unsigned i = 0; i < iterations; i++) {
            for (unsigned j = 0; j < count; j++) {
                demux.demux(src + SRC_SIZE * j, SRC_SIZE, dst + NUM_TIMESLOTS * j);
            }
        }
        int64_t t = (int64_t)(currentTimeMillis() - t0);
        if (count == MIN_COUNT) {
            tfirst = t;
        }
        else {
            t -= tfirst;
        }
        printf("%5d", t);
        fflush(stdout);
        iterations /= 2;
    }
    cout << endl;
}

void measure_read_uncached (const Demux & demux)
{
    printf(" read-%-30s:", typeid (demux).name());
    fflush(stdout);
    unsigned iterations = ITERATIONS;
    int64_t tfirst;

    for (unsigned count = MIN_COUNT; count <= MAX_COUNT; count *= 2) {
        uint64_t t0 = currentTimeMillis();
        for (unsigned i = 0; i < iterations; i++) {
            for (unsigned j = 0; j < count; j++) {
                demux.demux(src + SRC_SIZE * j, SRC_SIZE, dst);
            }
        }
        int64_t t = (int64_t)(currentTimeMillis() - t0);
        if (count == MIN_COUNT) {
            tfirst = t;
        }
        else {
            t -= tfirst;
        }
        printf("%5d", t);
        fflush(stdout);
        iterations /= 2;
    }
    cout << endl;
}

void measure_write_uncached (const Demux & demux)
{
    printf("write-%-30s:", typeid (demux).name());
    fflush(stdout);
    unsigned iterations = ITERATIONS;
    int64_t tfirst;

    for (unsigned count = MIN_COUNT; count <= MAX_COUNT; count *= 2) {
        uint64_t t0 = currentTimeMillis();
        for (unsigned i = 0; i < iterations; i++) {
            for (unsigned j = 0; j < count; j++) {
                demux.demux(src, SRC_SIZE, dst + NUM_TIMESLOTS * j);
            }
        }
        int64_t t = (int64_t)(currentTimeMillis() - t0);
        if (count == MIN_COUNT) {
            tfirst = t;
        }
        else {
            t -= tfirst;
        }
        printf("%5d", t);
        fflush(stdout);
        iterations /= 2;
    }
    cout << endl;
}

void measure_rand(const Demux & demux)
{
    printf("rand  %-30s:", typeid (demux).name());
    fflush(stdout);
    unsigned iterations = ITERATIONS;
    int64_t tfirst;
    srand(0);

    for (unsigned count = MIN_COUNT; count <= MAX_COUNT; count *= 2) {
        uint64_t t0 = currentTimeMillis();
        for (unsigned i = 0; i < iterations; i++) {
            for (unsigned j = 0; j < count; j++) {
                unsigned p = rand() & (count - 1);
                demux.demux(src + SRC_SIZE * p, SRC_SIZE, dst + NUM_TIMESLOTS * p);
            }
        }
        int64_t t = (int64_t)(currentTimeMillis() - t0);
        if (count == MIN_COUNT) {
            tfirst = t;
        }
        else {
            t -= tfirst;
        }
        printf("%5d", t);
        fflush(stdout);
        iterations /= 2;
    }
    cout << endl;
}

void measure(const Demux & demux)
{
    measure_base(demux);
    measure_rand(demux);
    printf("\n");
}

int main (void)
{
    src = generate (MAX_COUNT);
    dst = allocate_dst(MAX_COUNT);

    printf("      %30s:", "");
    for (size_t count = MIN_COUNT; count <= MAX_COUNT; count *= 2) {
        size_t size = count * SRC_SIZE * 2;
        char c = ' ';
        if (size >= 1024 * 1024 * 1024) {
            size /= 1024 * 1024 * 1024;
            c = 'g';
        } else if (size >= 1024 * 1024) {
            size /= 1024 * 1024;
            c = 'm';
        } else if (size >= 1024) {
            size /= 1024;
            c = 'k';
        }
        printf(" %3d%c", size, c);
    }
    printf("\n");

    measure (Src_First_1 ());
    measure (Dst_First_3a ());
    measure (Write8 ());
    measure (Read8_Write16_SSE_Unroll ());
    measure (Read8_Write32_AVX_Unroll ());
    measure (Copy_AVX ());

    return 0;
}
