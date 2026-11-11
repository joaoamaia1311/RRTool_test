#ifndef SMX_H
#define SMX_H

#include <ntddk.h>

/*
 * Wire contract for the SMX transport.
 *
 * The client composes request numbers with the same shift arithmetic
 * ((type << 16) | (function << 2)), method buffered, any access. Keep this
 * file and 2_src_port/rr_mem.c in step.
 */

#define SMX_TYPE        0x3BUL
#define SMX_CTL(fn)     ((ULONG)((SMX_TYPE << 16) | (((ULONG)(fn)) << 2)))

#define SMX_IO_READ     SMX_CTL(0x901)   /* read target memory               */
#define SMX_IO_BASE     SMX_CTL(0x902)   /* image base of the target         */
#define SMX_IO_UNLOAD   SMX_CTL(0x903)   /* drop the DOS link                */
#define SMX_IO_WRITE    SMX_CTL(0x904)   /* write target memory              */
#define SMX_IO_PEB      SMX_CTL(0x905)   /* PEB of the target                */
#define SMX_IO_REGION   SMX_CTL(0x906)   /* one region descriptor            */
#define SMX_IO_BATCH    SMX_CTL(0x907)   /* up to 64 reads in one call       */
#define SMX_IO_HIDE     SMX_CTL(0x908)   /* mask / unmask a process          */
#define SMX_IO_ALLOC    SMX_CTL(0x909)   /* allocate in the target           */
#define SMX_IO_APC      SMX_CTL(0x90A)   /* reserved                         */
#define SMX_IO_CAPS     SMX_CTL(0x90B)   /* reserved; no dispatch branch      */
#define SMX_IO_THREAD   SMX_CTL(0x90C)   /* reserved                         */
#define SMX_IO_CONTEXT  SMX_CTL(0x90D)   /* thread register snapshot         */

#define SMX_BATCH_MAX      64
#define SMX_MAX_TRANSFER   0x10000
#define SMX_HIDE_MAX       16

/* Standard request layout. Batch requests use SMX_BATCH instead; only the
 * magic_key and process_id prefix is shared by the two layouts. */
#pragma pack(push, 1)
typedef struct _SMX_CMD {
    ULONG     magic_key;
    ULONG     process_id;
    ULONG_PTR address;
    SIZE_T    size;
} SMX_CMD, *PSMX_CMD;

typedef struct _SMX_CTX {
    ULONGLONG rip, rsp, rbp, rflags;
    ULONG     tid;
    ULONG     ok;
} SMX_CTX, *PSMX_CTX;

typedef struct _SMX_BITEM {
    ULONGLONG address;
    ULONG     size;
    ULONG     offset;
} SMX_BITEM, *PSMX_BITEM;

typedef struct _SMX_BATCH {
    ULONG     magic_key;
    ULONG     process_id;
    ULONG     count;
    ULONG     total;                 /* reply data bytes from buffer offset 0 */
    SMX_BITEM items[SMX_BATCH_MAX];
    /* In the reply, one status byte per item follows at buffer + total.
     * The reply overwrites the request; it does not retain this header. */
} SMX_BATCH, *PSMX_BATCH;

typedef struct _SMX_REGION {
    ULONG_PTR base;
    ULONG_PTR size;
    ULONG     state;
    ULONG     protect;
    ULONG     type;
    ULONG     pad;
} SMX_REGION, *PSMX_REGION;
#pragma pack(pop)

/* Retained security descriptor: System has full access; administrators,
 * Everyone and restricted code receive read/write/execute permissions.
 * SmxInit() currently uses IoCreateDevice() and does not apply this constant. */
#define SMX_SDDL \
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)"

#endif /* SMX_H */
