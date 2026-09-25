/* Thin ARM NEON wrapper around ISA-L gf_*vect_dot_prod_neon. */

#include "erasure_code.h"

extern void gf_vect_dot_prod_neon(int len, int vlen, unsigned char* gftbls,
                                  unsigned char** src, unsigned char* dest);
extern void gf_2vect_dot_prod_neon(int len, int vlen, unsigned char* gftbls,
                                   unsigned char** src, unsigned char** dest);
extern void gf_3vect_dot_prod_neon(int len, int vlen, unsigned char* gftbls,
                                   unsigned char** src, unsigned char** dest);
extern void gf_4vect_dot_prod_neon(int len, int vlen, unsigned char* gftbls,
                                   unsigned char** src, unsigned char** dest);
extern void gf_5vect_dot_prod_neon(int len, int vlen, unsigned char* gftbls,
                                   unsigned char** src, unsigned char** dest);

void vstreamer_ec_encode_data(int len, int k, int rows, unsigned char* g_tbls,
                              unsigned char** data, unsigned char** coding)
{
    if (len < 16)
    {
        ec_encode_data_base(len, k, rows, g_tbls, data, coding);
        return;
    }

    while (rows > 5)
    {
        gf_5vect_dot_prod_neon(len, k, g_tbls, data, coding);
        g_tbls += 5 * k * 32;
        coding += 5;
        rows -= 5;
    }
    switch (rows)
    {
        case 5:
            gf_5vect_dot_prod_neon(len, k, g_tbls, data, coding);
            break;
        case 4:
            gf_4vect_dot_prod_neon(len, k, g_tbls, data, coding);
            break;
        case 3:
            gf_3vect_dot_prod_neon(len, k, g_tbls, data, coding);
            break;
        case 2:
            gf_2vect_dot_prod_neon(len, k, g_tbls, data, coding);
            break;
        case 1:
            gf_vect_dot_prod_neon(len, k, g_tbls, data, *coding);
            break;
        default:
            break;
    }
}
