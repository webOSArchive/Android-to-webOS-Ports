#include "linux_wrappers.h"

/* The loop used to stop when EITHER string ended and return 0, so a prefix
 * compared equal and "" equalled everything. Marmalade resolves each path
 * component with strcasecmp against readdir, and the empty component after
 * its data root's trailing slash "matched" the first entry: PvZ HD's saves
 * went to <root>/_manifest.json/userdata/ (ENOTDIR) and were silently lost.
 * Compare through the terminator, as libc does. */
int
my_strcasecmp(__const char *__s1, __const char *__s2)
{
#define TOLOWER(x) ((((x) >= 'A') && ((x) <= 'Z')) ? ((x) - 'A' + 'a') : (x))
    for(;;)
    {
        unsigned char c1 = TOLOWER((unsigned char)*__s1), c2 = TOLOWER((unsigned char)*__s2);
        if(c1 != c2) return (c1 - c2);
        if(c1 == '\0') return 0;
        __s1++;
        __s2++;
    }
#undef TOLOWER
}
