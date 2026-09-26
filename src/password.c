/* password.c - unlocking a player's turn file, for the library's dumps.
 *
 * A Stars! password is no more than a 32-bit hash in the player's own block,
 * which the game compares with the hash of whatever is typed when the file is
 * opened.  Blank that hash and there is nothing to type: the game opens the
 * file exactly as it would have with the right password, down to the byte in
 * every dump it writes - checked against the emulator given the password.
 *
 * The file is scrambled, but not so as to stand in the way.  Every block after
 * the header is XORed, four bytes at a time, with a keystream from a pair of
 * multiplicative generators seeded by the header, running on from one block
 * to the next.  So the hash can be read by running the keystream up to it,
 * and changed by XORing the difference into the file's own bytes; nothing
 * else moves, and there is no checksum to put right.  The format is as
 * starsapi documents it (StarsHostEditor before it): Decryptor.java,
 * StarsRandom.java, FileHeaderBlock.java and PlayerBlock.java.
 */

#include "lib.h"

/* The seeds: the first 64 odd primes, except that the 56th is 279, which is
   not a prime.  That is the game's own table and the one its files use. */
static const uint16_t primes[64] = {
      3,   5,   7,  11,  13,  17,  19,  23,
     29,  31,  37,  41,  43,  47,  53,  59,
     61,  67,  71,  73,  79,  83,  89,  97,
    101, 103, 107, 109, 113, 127, 131, 137,
    139, 149, 151, 157, 163, 167, 173, 179,
    181, 191, 193, 197, 199, 211, 223, 227,
    229, 233, 239, 241, 251, 257, 263, 279,
    271, 277, 281, 283, 293, 307, 311, 313,
};

typedef struct { int64_t a, b; } Keys;

/* Two of L'Ecuyer's multiplicative generators, and their difference. */
static uint32_t key_next(Keys *k)
{
    int64_t a = (k->a % 53668) * 40014 - (k->a / 53668) * 12211;
    int64_t b = (k->b % 52774) * 40692 - (k->b / 52774) * 3791;
    if (a < 0) a += 0x7FFFFFAB;
    if (b < 0) b += 0x7FFFFF07;
    k->a = a;
    k->b = b;
    return (uint32_t)(a - b);
}

static unsigned pw_rd16(const uint8_t *p)
{
    return (unsigned)(p[0] | p[1] << 8);
}
static uint32_t pw_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* The keystream for a file whose header block holds `h`: 16 bytes, of which
   the game id, the turn, the player and the salt pick the seeds and how far
   along they start. */
static Keys keys_for(const uint8_t *h)
{
    unsigned turn = pw_rd16(h + 10), pdata = pw_rd16(h + 12);
    unsigned salt = pdata >> 5, player = pdata & 0x1F;
    unsigned i1 = salt & 0x1F, i2 = (salt >> 5) & 0x1F, rounds, n;
    Keys k;

    if (salt >> 10) i1 += 32;
    else i2 += 32;
    rounds = ((pw_rd32(h + 4) & 3) + 1) * ((turn & 3) + 1) * ((player & 3) + 1)
           + ((h[15] >> 4) & 1);                    /* shareware */
    k.a = primes[i1];
    k.b = primes[i2];
    for (n = 0; n < rounds; n++) key_next(&k);
    return k;
}

int password_blank(uint8_t *d, ptrdiff_t len)
{
    ptrdiff_t off = 0;
    unsigned owner = 0x20;               /* none until the header says */
    Keys k = {0, 0};
    int have_keys = 0;

    while (off + 2 <= len) {
        unsigned hdr = pw_rd16(d + off);
        unsigned type = hdr >> 10, size = hdr & 0x3FF, i;
        uint8_t *body = d + off + 2;
        uint8_t plain[16] = {0};

        if ((ptrdiff_t)size > len - off - 2) return -1;
        if (type == 8) {                              /* the header */
            if (size < 16) return -1;
            k = keys_for(body);
            have_keys = 1;
            owner = pw_rd16(body + 12) & 0x1F;
        } else if (type != 0) {                       /* 0 is the footer */
            if (!have_keys) return -1;
            /* Every four bytes of the block, padded, take a key, and only
               the first sixteen are ever wanted in the clear. */
            for (i = 0; i < size; i += 4) {
                uint32_t key = key_next(&k);
                unsigned j;
                for (j = 0; j < 4; j++) {
                    uint8_t kb = (uint8_t)(key >> (8 * j));
                    uint8_t cb = i + j < size ? body[i + j] : 0;
                    if (i + j < 16) plain[i + j] = (uint8_t)(cb ^ kb);
                }
                /* The owner's own block: bytes 12 to 15 are the hash, and a
                   blank one is zero.  The same goes for a player the host
                   has made inactive, whose hash is kept inverted so that no
                   password will match it until they come back: zero opens
                   that file too, as the emulator confirms. */
                if (type == 6 && i == 12 && size >= 16 && plain[0] == owner) {
                    for (j = 0; j < 4; j++)
                        body[12 + j] = (uint8_t)(key >> (8 * j));
                    return 1;
                }
            }
        }
        off += 2 + (ptrdiff_t)size;
        /* The planets block is followed by four bytes a planet, outside it
           and in the clear: the count is in the block, at offset 10. */
        if (type == 7 && size >= 12)
            off += (ptrdiff_t)pw_rd16(plain + 10) * 4;
    }
    return 0;
}
