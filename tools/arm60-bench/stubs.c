/* Stand-ins for routines the 3DO resolves from the kernel ROM, so the bench
 * links without the OS. memset is only used once, outside the timed frames. */
#include <stddef.h>
void __rt_stkovf_split_small(void) { }
void *memset(void *d,int c,size_t n)
{
    unsigned char *a=(unsigned char *)d;
    while (n--) *a++=(unsigned char)c;
    return d;
}
