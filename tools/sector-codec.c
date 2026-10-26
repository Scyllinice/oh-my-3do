/* Original MIT-licensed ECMA-130 Mode 1 sector encoder.
 * Stateless, allocation-free ABI used only by the PC scanner. No CRT imports
 * are needed in the Windows DLL. The input and output buffers must not overlap.
 *
 * A cooked .iso holds only the 2048 data bytes of each CD sector; the disc
 * stores 2352 (Mode 1): a sync pattern, the sector's address, the data, an
 * EDC checksum and Reed-Solomon P and Q parity. Disc databases list the
 * checksums of the full sectors, so the library organizer rebuilds them to
 * identify an .iso. sector-codec.py has the same encoder in Python (used
 * when this library isn't built) and loads this one with ctypes.
 *
 * Exported functions:
 *   oh3do_sector_abi()    the interface version, 1
 *   oh3do_mode1_encode()  rebuild up to 512 sectors
 */
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef unsigned char Byte;
typedef unsigned int Word;
/* Fails to compile unless Word is 32 bits. */
typedef char require_32_bit_word[sizeof(Word) == 4 ? 1 : -1];

#define COOKED_BYTES 2048
#define RAW_BYTES    2352

/* The most sectors per call, and the end of the address range (99:59:74). */
#define MAX_BATCH   512
#define MAX_SECTORS 449850

/* Data tracks start two seconds (150 sectors) in: sector 0 is 00:02:00. */
#define PREGAP_SECTORS     150
#define SECTORS_PER_SECOND 75
#define SECTORS_PER_MINUTE (75 * 60)

/* Where each part sits in a 2352-byte sector. */
#define SYNC_BYTES      12
#define HEADER_OFFSET   12 /* minutes, seconds, frames (BCD), mode */
#define DATA_OFFSET     16
#define EDC_OFFSET      2064 /* little-endian CRC, then 8 zero bytes */
#define ZERO_OFFSET     2068
#define P_PARITY_OFFSET 2076
#define Q_PARITY_OFFSET 2248

/* The EDC's CRC-32 polynomial (bit-reversed) and the Galois field's. */
#define EDC_POLYNOMIAL    0xd8018001u
#define GALOIS_POLYNOMIAL 0x11d

/* `value` multiplied by 2 in GF(256). */
static Byte twice(Byte value)
{
    Word product = (Word)value << 1;
    if (value & 128)
        product ^= GALOIS_POLYNOMIAL;
    return (Byte)product;
}

/* Two RS parity symbols over GF(256), roots 1 and 2. P and Q use the
 * interleaving specified by ECMA-130 Annex A. The two syndromes determine
 * parity uniquely: p0 ^ p1 = s0; 2*p0 ^ p1 = 2*s1.
 *
 * The bytes from the header on are read as a grid of 16-bit words, `columns`
 * wide and `rows` deep: each column starts `column_step` words further on,
 * and each row `row_step` bytes further on, wrapping around the grid (which
 * makes Q's columns diagonals). The column's two parity bytes go to
 * `parity_offset` + column and `parity_offset` + columns + column.
 */
static void add_parity(Byte *sector, Word columns, Word rows, Word column_step, Word row_step,
                       Word parity_offset, const Byte *divide_by_three)
{
    Word column, row, position;
    Word grid_size = columns * rows;
    Byte s0, s1, value, p0;
    for (column = 0; column < columns; ++column) {
        /* Even and odd columns are the high and low bytes of one word. */
        position = (column / 2) * column_step + column % 2;
        s0 = 0;
        s1 = 0;
        for (row = 0; row < rows; ++row) {
            value = sector[HEADER_OFFSET + position];
            s0 ^= value;
            s1 = twice((Byte)(s1 ^ value));
            position = (position + row_step) % grid_size;
        }
        p0 = divide_by_three[twice(s1) ^ s0];
        sector[parity_offset + column] = p0;
        sector[parity_offset + columns + column] = p0 ^ s0;
    }
}

/* Binary-coded decimal: 42 becomes 0x42. */
static Byte bcd(Word value)
{
    return (Byte)((value / 10) * 16 + value % 10);
}

/* The lookup tables: crc_table[b] is the EDC CRC of byte b, and
 * divide_by_three[b] is b / 3 in GF(256) (3 = 2 + 1, so the value v with
 * twice(v) ^ v == b). */
static void build_tables(Word *crc_table, Byte *divide_by_three)
{
    Word i, bit, crc;
    for (i = 0; i < 256; ++i) {
        crc = i;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? EDC_POLYNOMIAL : 0);
        crc_table[i] = crc;
        divide_by_three[twice((Byte)i) ^ i] = (Byte)i;
    }
}

/* The 16 bytes before the data: sync (00, ten FFs, 00), then the sector's
 * address as minutes, seconds and frames in BCD, then mode 1. */
static void write_header(Byte *sector, Word sector_number)
{
    Word i;
    Word address = sector_number + PREGAP_SECTORS;
    sector[0] = 0;
    for (i = 1; i < SYNC_BYTES - 1; ++i)
        sector[i] = 255;
    sector[SYNC_BYTES - 1] = 0;
    sector[HEADER_OFFSET] = bcd(address / SECTORS_PER_MINUTE);
    sector[HEADER_OFFSET + 1] = bcd((address / SECTORS_PER_SECOND) % 60);
    sector[HEADER_OFFSET + 2] = bcd(address % SECTORS_PER_SECOND);
    sector[HEADER_OFFSET + 3] = 1;
}

/* The EDC: a CRC-32 over the sync, header and data, stored little-endian,
 * followed by 8 zero bytes. */
static void write_edc(Byte *sector, const Word *crc_table)
{
    Word i, crc = 0;
    for (i = 0; i < EDC_OFFSET; ++i)
        crc = (crc >> 8) ^ crc_table[(crc ^ sector[i]) & 255];
    for (i = 0; i < 4; ++i)
        sector[EDC_OFFSET + i] = (Byte)(crc >> (i * 8));
    for (i = ZERO_OFFSET; i < P_PARITY_OFFSET; ++i)
        sector[i] = 0;
}

EXPORT Word oh3do_sector_abi(void)
{
    return 1;
}

/* Rebuild `count` full 2352-byte sectors into `output` from as many 2048-byte
 * sectors in `input`, the first being sector `first` of the image. Returns
 * 0, or -1 for missing buffers or a range beyond MAX_BATCH or the disc. */
EXPORT int oh3do_mode1_encode(const Byte *input, Word count, Word first, Byte *output)
{
    Word crc_table[256], i, n;
    Byte divide_by_three[256], *sector;
    if (!input || !output || count > MAX_BATCH || first > MAX_SECTORS ||
        count > MAX_SECTORS - first)
        return -1;
    build_tables(crc_table, divide_by_three);

    for (n = 0; n < count; ++n) {
        sector = output + n * RAW_BYTES;
        write_header(sector, first + n);
        for (i = 0; i < COOKED_BYTES; ++i)
            sector[DATA_OFFSET + i] = input[n * COOKED_BYTES + i];
        write_edc(sector, crc_table);
        /* P: 86 columns of 24 bytes, straight down, covering the header to
         * the zero bytes. Q: 52 diagonals of 43 bytes, covering those and P. */
        add_parity(sector, 86, 24, 2, 86, P_PARITY_OFFSET, divide_by_three);
        add_parity(sector, 52, 43, 86, 88, Q_PARITY_OFFSET, divide_by_three);
    }
    return 0;
}
