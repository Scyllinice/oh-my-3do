/* Original MIT-licensed ECMA-130 Mode 1 sector encoder.
 * Stateless, allocation-free ABI used only by the PC scanner. No CRT imports
 * are needed in the Windows DLL. The input and output buffers must not overlap.
 */
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef unsigned char Byte;
typedef unsigned int Word;
typedef char require_32_bit_word[sizeof(Word) == 4 ? 1 : -1];

static Byte twice(Byte value)
{
    return (Byte)(((Word)value << 1) ^ ((value & 128) ? 0x11d : 0));
}

/* Two RS parity symbols over GF(256), roots 1 and 2. P and Q use the
 * interleaving specified by ECMA-130 Annex A. The two syndromes determine
 * parity uniquely: p0 ^ p1 = s0; 2*p0 ^ p1 = 2*s1.
 */
static void parity(Byte *sector, Word columns, Word rows, Word step, Word stride, Word output,
                   const Byte *divide_three)
{
    Word column, row, position, span = columns * rows;
    Byte s0, s1, value, p0;
    for (column = 0; column < columns; ++column) {
        position = (column / 2) * step + column % 2;
        s0 = s1 = 0;
        for (row = 0; row < rows; ++row) {
            value = sector[12 + position];
            s0 ^= value;
            s1 = twice((Byte)(s1 ^ value));
            position = (position + stride) % span;
        }
        p0 = divide_three[twice(s1) ^ s0];
        sector[output + column] = p0;
        sector[output + columns + column] = p0 ^ s0;
    }
}

static Byte bcd(Word value)
{
    return (Byte)((value / 10) * 16 + value % 10);
}

EXPORT Word oh3do_sector_abi(void)
{
    return 1;
}

EXPORT int oh3do_mode1_encode(const Byte *input, Word count, Word first, Byte *output)
{
    Word crc_table[256], i, bit, crc, frame, n;
    Byte divide_three[256], *sector;
    if (!input || !output || count > 512 || first > 449850 || count > 449850 - first)
        return -1;
    for (i = 0; i < 256; ++i) {
        crc = i;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xd8018001u : 0);
        crc_table[i] = crc;
        divide_three[twice((Byte)i) ^ i] = (Byte)i;
    }
    for (n = 0; n < count; ++n) {
        sector = output + n * 2352;
        sector[0] = sector[11] = 0;
        for (i = 1; i < 11; ++i)
            sector[i] = 255;
        frame = first + n + 150; /* LBA 0 is 00:02:00. */
        sector[12] = bcd(frame / 4500);
        sector[13] = bcd((frame / 75) % 60);
        sector[14] = bcd(frame % 75);
        sector[15] = 1;
        for (i = 0; i < 2048; ++i)
            sector[16 + i] = input[n * 2048 + i];
        crc = 0;
        for (i = 0; i < 2064; ++i)
            crc = (crc >> 8) ^ crc_table[(crc ^ sector[i]) & 255];
        for (i = 0; i < 4; ++i)
            sector[2064 + i] = (Byte)(crc >> (i * 8));
        for (i = 2068; i < 2076; ++i)
            sector[i] = 0;
        parity(sector, 86, 24, 2, 86, 2076, divide_three);
        parity(sector, 52, 43, 86, 88, 2248, divide_three);
    }
    return 0;
}
