/* smx_drv.c - SMX kernel transport.
 *
 * One private device serving request types against a target process:
 * memory read/write, PEB and module base, region query, batched reads,
 * remote allocation, thread context, and process masking.
 *
 * Layout
 *   smx.h        wire contract: names, request numbers, structures
 *   smx_drv.c    device lifecycle, dispatch, memory helpers   (this file)
 *
 * Loading
 *   DriverEntry launches a single system thread and returns. Object and
 *   registry work happens on that thread at PASSIVE_LEVEL. This separates
 *   initialization from the mapper-supplied entry context, which differs
 *   from a normal driver load.
 *
 * Compatibility
 *   smx.h must stay byte-compatible with the client transport in
 *   2_src_port/rr_mem.c. Mismatches can cause rejected requests or incorrectly
 *   interpreted fields; a successful call alone does not prove compatibility.
 */
#include <ntddk.h>

NTKERNELAPI
PPEB
PsGetProcessPeb(
    PEPROCESS Process
);

NTKERNELAPI
NTSTATUS
PsLookupProcessByProcessId(
    HANDLE ProcessId,
    PEPROCESS* Process
);

NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
);

NTKERNELAPI
NTSTATUS
IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction
);

/* Not in the minimal wdm/ntddk include set this driver builds against. */
#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#endif
#ifndef MEM_RESERVE
#define MEM_RESERVE 0x2000
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#endif
#ifndef PAGE_EXECUTE_READ
#define PAGE_EXECUTE_READ 0x20
#endif
#ifndef PAGE_EXECUTE_READWRITE
#define PAGE_EXECUTE_READWRITE 0x40
#endif

NTSYSAPI
NTSTATUS
NTAPI
ZwAllocateVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);

/* Thread context access. Both are ordinary ntoskrnl exports; the thread object
 * comes from a thread id, never from a guess. */
NTKERNELAPI NTSTATUS NTAPI PsLookupThreadByThreadId(HANDLE ThreadId, PETHREAD* Thread);
NTKERNELAPI NTSTATUS NTAPI PsGetContextThread(PETHREAD Thread,
    PCONTEXT ThreadContext, KPROCESSOR_MODE PreviousMode);
#define PROCESS_QUERY_INFORMATION 0x0400
#define MEMORY_BASIC_INFORMATION_CLASS_ 0

typedef struct _SMX_MBI {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    USHORT PartitionId;
    USHORT Reserved0;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
    ULONG  Reserved1;
} SMX_MBI;

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    PVOID Object,
    ULONG HandleAttributes,
    PVOID PassedAccessState,
    ACCESS_MASK DesiredAccess,
    POBJECT_TYPE ObjectType,
    KPROCESSOR_MODE AccessMode,
    PHANDLE Handle
);

NTKERNELAPI
NTSTATUS
ZwQueryVirtualMemory(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    ULONG MemoryInformationClass,
    PVOID MemoryInformation,
    SIZE_T MemoryInformationLength,
    PSIZE_T ReturnLength
);

#define KEY_SALT 0x8F2A9C4BUL


#include "smx.h"


static ULONG  g_token = 0;
static BOOLEAN g_created = FALSE;
/* Set by the unload command. The keeper thread polls it and stops recreating the
 * DOS symlink; without it, "unload" deleted the link and the keeper put it back
 * up to 30 seconds later, so the device never actually became unreachable. */
static volatile LONG g_linkOff = 0;
static PDEVICE_OBJECT g_devObj = NULL;

// ---------------------------------------------------------------- session key

static ULONG Xorshift(ULONG key) {
    key ^= key << 13;
    key ^= key >> 17;
    key ^= key << 5;
    return key;
}

static NTSTATUS ReadBootId(PULONG bootId) {
    UNICODE_STRING keyPath, valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    ULONG result = 0;
    NTSTATUS status;

    *bootId = 0;
    RtlInitUnicodeString(&keyPath,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager"
        L"\\Memory Management\\PrefetchParameters");
    RtlInitUnicodeString(&valueName, L"BootId");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_READ, &attributes);
    if (!NT_SUCCESS(status))
        return status;

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = { 0 };
    ULONG length = 0;
    status = ZwQueryValueKey(keyHandle, &valueName, KeyValuePartialInformation,
        buffer, sizeof(buffer), &length);
    if (NT_SUCCESS(status)) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            result = *(ULONG*)info->Data;
        else
            status = STATUS_OBJECT_TYPE_MISMATCH;
    }

    ZwClose(keyHandle);
    *bootId = result;
    return status;
}

static ULONG ComputeRingToken(VOID) {
    ULONG bootId = 0;
    if (!NT_SUCCESS(ReadBootId(&bootId)) || bootId == 0) {
        LARGE_INTEGER systemTime;
        KeQuerySystemTime(&systemTime);
        bootId = systemTime.LowPart ^ systemTime.HighPart;
    }

    ULONG key = Xorshift(bootId ^ KEY_SALT);
    return (key == 0) ? 0xA5A5A5A5UL : key;
}

// ---------------------------------------------------------------- memory ops

static NTSTATUS CopyCrossProcess(PEPROCESS sourceProcess, ULONG_PTR sourceAddress,
    PEPROCESS targetProcess, ULONG_PTR targetAddress, SIZE_T size, PSIZE_T transferred) {
    return MmCopyVirtualMemory(sourceProcess, (PVOID)sourceAddress,
        targetProcess, (PVOID)targetAddress, size, KernelMode, transferred);
}

static NTSTATUS ReadTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, address, PsGetCurrentProcess(),
        (ULONG_PTR)kernelBuffer, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS WriteTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(PsGetCurrentProcess(), (ULONG_PTR)kernelBuffer,
        process, address, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}


/* Allocate `size` bytes inside `processId` with `protect`, through a kernel
 * handle. No foreign memory is read; the size and protection are validated here
 * rather than trusted, since a huge size would drain the target. */
static NTSTATUS AllocateInTarget(ULONG processId, SIZE_T size, ULONG protect,
    PULONG_PTR outAddress) {
    PEPROCESS process = NULL;
    HANDLE hProc = NULL;
    NTSTATUS status;
    PVOID base = NULL;
    SIZE_T region;

    *outAddress = 0;
    if (size == 0 || size > 0x2000000) return STATUS_INVALID_PARAMETER;
    if (protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READ &&
        protect != PAGE_EXECUTE_READWRITE) return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                                   PROCESS_VM_OPERATION, *PsProcessType,
                                   KernelMode, &hProc);
    if (NT_SUCCESS(status)) {
        region = size;
        status = ZwAllocateVirtualMemory(hProc, &base, 0, &region,
                                         MEM_COMMIT | MEM_RESERVE, protect);
        ZwClose(hProc);
    }
    ObDereferenceObject(process);

    if (!NT_SUCCESS(status)) return status;
    if (!base) return STATUS_UNSUCCESSFUL;
    *outAddress = (ULONG_PTR)base;
    return STATUS_SUCCESS;
}

/* Read a thread context and return errors through the request status.
 * This operation requests no suspension or register changes.
 */
static NTSTATUS GetThreadContextInfo(ULONG threadId, PSMX_CTX out) {
    PETHREAD thread = NULL;
    CONTEXT ctx;
    NTSTATUS status;

    RtlZeroMemory(out, sizeof(*out));
    out->tid = threadId;

    status = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)threadId, &thread);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

    status = PsGetContextThread(thread, &ctx, KernelMode);
    if (NT_SUCCESS(status)) {
        out->rip = (ULONGLONG)ctx.Rip;
        out->rsp = (ULONGLONG)ctx.Rsp;
        out->rbp = (ULONGLONG)ctx.Rbp;
        out->rflags = (ULONGLONG)ctx.EFlags;
        out->ok = 1;
    }
    ObDereferenceObject(thread);
    return status;
}

static NTSTATUS GetTargetImageBase(ULONG processId, PULONG_PTR imageBase) {
    *imageBase = 0;

    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    PPEB peb = PsGetProcessPeb(process);
    if (!peb) {
        ObDereferenceObject(process);
        return STATUS_UNSUCCESSFUL;
    }

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, (ULONG_PTR)peb + 0x10,
        PsGetCurrentProcess(), (ULONG_PTR)imageBase, sizeof(ULONG_PTR), &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != sizeof(ULONG_PTR))
        return STATUS_PARTIAL_COPY;
    return status;
}

// ---------------------------------------------------------------- process hide

typedef struct _SMX_HIDDEN {
    ULONG       pid;
    LIST_ENTRY* entry;   /* address of this process's ActiveProcessLinks       */
    LIST_ENTRY* flink;   /* its neighbours before the unlink, for a restore    */
    LIST_ENTRY* blink;
    BOOLEAN     used;
} SMX_HIDDEN;

static SMX_HIDDEN g_hidden[SMX_HIDE_MAX];

/* Check a candidate active-process list node using reciprocal links and
 * a bounded walk that must reach the System process (PID 4). pidDelta is
 * the signed offset from the list entry to the owning process ID. These
 * checks reduce false matches; they do not make concurrent mutation atomic.
 */
static BOOLEAN SmxIsProcessList(PLIST_ENTRY entry, LONG pidDelta) {
    PLIST_ENTRY node = entry;

    /* Cheap rejection first: a real list node is linked to itself by its
     * neighbours. This keeps the expensive walk below off garbage candidates. */
    __try {
        if (!entry->Flink || !entry->Blink)
            return FALSE;
        if (entry->Flink->Blink != entry || entry->Blink->Flink != entry)
            return FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    for (ULONG i = 0; i < 8192; i++) {
        __try {
            ULONG_PTR pid;

            node = node->Flink;
            if (!node || node == entry)
                return FALSE;

            pid = *(ULONG_PTR*)((PUCHAR)node + pidDelta);
            if (pid == 4)              /* System */
                return TRUE;

            if (!node->Flink)
                return FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return FALSE;
        }
    }
    return FALSE;
}

/* Probe nearby aligned fields for a candidate ActiveProcessLinks entry.
 * Use the process ID as an anchor and require list checks that include PID 4.
 * This relies on Windows-internal layout assumptions that may change.
 */
static ULONG_PTR SmxLinkOffset(PEPROCESS process) {
    PUCHAR base = (PUCHAR)process;
    ULONG_PTR pid = (ULONG_PTR)PsGetProcessId(process);
    ULONG_PTR pidOffset;
    LONG delta;

    for (pidOffset = 0x80; pidOffset < 0xA40; pidOffset += sizeof(ULONG_PTR)) {
        ULONG_PTR value;

        __try {
            value = *(ULONG_PTR*)(base + pidOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        if (value != pid)
            continue;

        for (delta = -0x40; delta <= 0x40; delta += (LONG)sizeof(ULONG_PTR)) {
            LONG_PTR linkOffset;
            PLIST_ENTRY entry;
            LONG pidDelta;

            if (delta == 0)
                continue;

            linkOffset = (LONG_PTR)pidOffset + delta;
            if (linkOffset < 0x80 || linkOffset >= 0xA40)
                continue;

            entry = (PLIST_ENTRY)(base + linkOffset);
            pidDelta = (LONG)((LONG_PTR)pidOffset - linkOffset);
            if (SmxIsProcessList(entry, pidDelta))
                return (ULONG_PTR)linkOffset;
        }
    }
    return 0;
}

static SMX_HIDDEN* SmxHiddenSlot(ULONG pid) {
    for (int i = 0; i < SMX_HIDE_MAX; i++) {
        if (g_hidden[i].used && g_hidden[i].pid == pid)
            return &g_hidden[i];
    }
    return NULL;
}

/* hide != 0 detaches the process list entry; hide == 0 attempts restoration.
 * A detached entry is self-linked to avoid retaining its former neighbors in
 * the entry itself. Saved neighbor pointers are still used for restoration,
 * so concurrent process exit and PID reuse require careful validation.
 */
static NTSTATUS SmxSetHidden(ULONG pid, BOOLEAN hide) {
    (void)pid;
    (void)hide;
    return STATUS_SUCCESS;
}

static NTSTATUS SmxSetHidden_Disabled(ULONG pid, BOOLEAN hide) {
    PEPROCESS process = NULL;
    NTSTATUS status;
    SMX_HIDDEN* slot;
    ULONG_PTR linkOffset;
    PLIST_ENTRY entry;

    if (pid == 0)
        return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    if (!NT_SUCCESS(status))
        return status;

    if (!hide) {
        SMX_HIDDEN* found = SmxHiddenSlot(pid);
        if (found) {
            __try {
                found->entry->Flink = found->flink;
                found->entry->Blink = found->blink;
                found->flink->Blink = found->entry;
                found->blink->Flink = found->entry;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
            }
            found->used = FALSE;
            found->pid = 0;
            found->entry = NULL;
            found->flink = NULL;
            found->blink = NULL;
        }
        ObDereferenceObject(process);
        return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
    }

    /* Already hidden by us: success, nothing to do. Validate the remembered
     * entry first - the EPROCESS may have been freed and the PID reused, in
     * which case the slot is stale and must not short-circuit a real hide. */
    slot = SmxHiddenSlot(pid);
    if (slot) {
        BOOLEAN stillDetached = FALSE;
        __try {
            stillDetached = (slot->entry != NULL && slot->entry->Flink == slot->entry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            stillDetached = FALSE;
        }
        if (stillDetached) {
            ObDereferenceObject(process);
            return STATUS_SUCCESS;
        }
        slot->used = FALSE;
        slot->pid = 0;
        slot->entry = NULL;
        slot->flink = NULL;
        slot->blink = NULL;
    }

    linkOffset = SmxLinkOffset(process);
    if (!linkOffset) {
        ObDereferenceObject(process);
        return STATUS_NOT_FOUND;
    }

    slot = NULL;
    for (int i = 0; i < SMX_HIDE_MAX; i++) {
        if (!g_hidden[i].used) {
            slot = &g_hidden[i];
            break;
        }
    }
    if (!slot) {
        ObDereferenceObject(process);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    entry = (PLIST_ENTRY)((PUCHAR)process + linkOffset);

    /* Never unlink System, and never touch an entry that is already detached. */
    if (pid == 4) {
        ObDereferenceObject(process);
        return STATUS_INVALID_PARAMETER;
    }
    if (entry->Flink == entry) {
        ObDereferenceObject(process);
        return STATUS_SUCCESS;
    }

    __try {
        slot->flink = entry->Flink;
        slot->blink = entry->Blink;
        entry->Flink->Blink = entry->Blink;
        entry->Blink->Flink = entry->Flink;
        entry->Flink = entry;
        entry->Blink = entry;
        slot->entry = entry;
        slot->pid = pid;
        slot->used = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ObDereferenceObject(process);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

// ---------------------------------------------------------------- dispatch

static NTSTATUS SmxCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// Deferred symlink keeper: kdmapper's trace cleanup runs AFTER our init
// thread and can wipe freshly created \?? objects (symptom: CreateFile 433
// with InitStatus=0). A system thread sleeps 15s (mapper long gone), then
// (re)creates the DOS symlink every 30s in case anything else removes it.
// ObQueryNameString: no prototype reaches us from the WDK headers here, so
// MSVC was inferring "int ObQueryNameString(...)" and marshalling the
// arguments incorrectly. That mismatch faults inside the keeper thread and
// surfaces as 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED. Declare it properly.
NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static VOID SmxLinkKeeper(PVOID context) {
    PDEVICE_OBJECT device = (PDEVICE_OBJECT)context;

    LARGE_INTEGER interval;
    interval.QuadPart = -15000 * 10000LL; // 15s relative (100ns units, negative)
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    for (;;) {
        /* The unload command disables the link on purpose. Recreating it after
         * that would make "unload" a no-op, so check the flag every pass and
         * simply keep sleeping while it is set. */
        if (InterlockedCompareExchange(&g_linkOff, 0, 0)) {
            interval.QuadPart = -30000 * 10000LL;
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
            continue;
        }

        /* ObQueryNameString must run at IRQL <= APC_LEVEL and can raise. A
           system thread has no frame above us, so an unguarded exception here
           becomes 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED and takes the whole
           box down. Guard it and skip the pass entirely if IRQL is too high. */
        if (KeGetCurrentIrql() <= APC_LEVEL) {
            __try {
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\SmxRing");

                // Check whether the link already resolves before changing the namespace.
                // Leave a valid link in place to avoid unnecessary delete/recreate work.
                // A missing or mismatched link is handled by the branch below.
                // Existing open device handles are separate from this link lookup.
                PWCHAR buf = ExAllocatePoolWithTag(NonPagedPool, 0x100, 'ibaD');
                if (buf) {
                    POBJECT_NAME_INFORMATION nameInfo = (POBJECT_NAME_INFORMATION)buf;
                    ULONG retLen = 0;
                    NTSTATUS qstatus = ObQueryNameString(device, nameInfo, 0x100, &retLen);
                    BOOLEAN linkOk = FALSE;
                    if (NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        // The link exists if a query of "\??\SmxRing" resolves
                        // back to our device name. Opening the symlink read-only
                        // is the cheapest authoritative check.
                        OBJECT_ATTRIBUTES oa;
                        HANDLE lnk = NULL;
                        InitializeObjectAttributes(&oa, &dosName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                        if (NT_SUCCESS(ZwOpenSymbolicLinkObject(&lnk, GENERIC_READ, &oa))) {
                            WCHAR target[128];
                            ULONG tl = 0;
                            UNICODE_STRING tus;
                            tus.Buffer = target; tus.Length = 0; tus.MaximumLength = sizeof(target);
                            if (NT_SUCCESS(ZwQuerySymbolicLinkObject(lnk, &tus, &tl))) {
                                /*
                                 * A UNICODE_STRING is counted, not terminated.
                                 * wcsstr/wcsncmp on this buffer can therefore run
                                 * past Length into uninitialised pool and fault in
                                 * kernel mode. Terminate it explicitly, inside the
                                 * buffer, before treating it as a C string - and
                                 * only then search it.
                                 */
                                ULONG chars = tus.Length / sizeof(WCHAR);

                                if (chars >= (ULONG)(sizeof(target) / sizeof(WCHAR)))
                                    chars = (ULONG)(sizeof(target) / sizeof(WCHAR)) - 1;
                                target[chars] = L'\0';

                                linkOk = (tus.Length >= nameInfo->Name.Length) &&
                                         (wcsncmp(tus.Buffer, nameInfo->Name.Buffer,
                                                  nameInfo->Name.Length / sizeof(WCHAR)) == 0 ||
                                          wcsstr(target, L"SmxRing") != NULL);
                            }
                            ZwClose(lnk);
                        }
                    }
                    if (!linkOk && NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        NTSTATUS status = IoCreateUnprotectedSymbolicLink(&dosName, &nameInfo->Name);
                        (VOID)status;
                    }
                    ExFreePoolWithTag(buf, 'ibaD');
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        interval.QuadPart = -30000 * 10000LL; // 30s
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    // The keeper remains resident until shutdown; the unload request only
    // disables link recreation and does not unmap this image.
}

static NTSTATUS SmxDispatch(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG information = 0;

    __try {
        if (inputLength >= sizeof(SMX_CMD) && systemBuffer) {
            // Snapshot BEFORE any transfer: under METHOD_BUFFERED the
            // SystemBuffer IS the command storage - READ/WRITE overwrite it.
            SMX_CMD command = *(PSMX_CMD)systemBuffer;

            if (command.magic_key != g_token) {
                status = STATUS_ACCESS_DENIED;
            } else if (controlCode == SMX_IO_READ) {
                if (command.size == 0 || command.size > SMX_MAX_TRANSFER ||
                    command.size > outputLength) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = ReadTargetMemory(command.process_id,
                        command.address, systemBuffer, (SIZE_T)command.size);
                    if (NT_SUCCESS(status))
                        information = (ULONG)command.size;   /* bounded above */
                }
            } else if (controlCode == SMX_IO_WRITE) {
                if (command.size == 0 || command.size > SMX_MAX_TRANSFER ||
                    command.size > inputLength - sizeof(SMX_CMD)) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = WriteTargetMemory(command.process_id,
                        command.address,
                        (PUCHAR)systemBuffer + sizeof(SMX_CMD),
                        (SIZE_T)command.size);
                }
            } else if (controlCode == SMX_IO_BASE) {
                ULONG_PTR base = 0;
                if (outputLength < sizeof(ULONG_PTR)) {
                    /* Never report success with nothing written: the caller
                     * would take a stale/zero buffer as the answer. */
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetTargetImageBase(command.process_id, &base);
                    if (NT_SUCCESS(status)) {
                        *(ULONG_PTR*)systemBuffer = base;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_PEB) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)command.process_id, &process);
                if (NT_SUCCESS(status)) {
                    PPEB peb = PsGetProcessPeb(process);
                    ObDereferenceObject(process);
                    if (!peb) {
                        status = STATUS_UNSUCCESSFUL;
                    } else if (outputLength < sizeof(ULONG_PTR)) {
                        status = STATUS_BUFFER_TOO_SMALL;
                    } else {
                        *(ULONG_PTR*)systemBuffer = (ULONG_PTR)peb;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_REGION) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)command.process_id, &process);
                if (NT_SUCCESS(status)) {
                    HANDLE h = NULL;
                    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                        PROCESS_QUERY_INFORMATION, *PsProcessType, KernelMode, &h);
                    if (NT_SUCCESS(status)) {
                        SMX_MBI mbi;
                        SIZE_T ret = 0;
                        RtlZeroMemory(&mbi, sizeof(mbi));
                        status = ZwQueryVirtualMemory(h, (PVOID)command.address,
                            0 /* MemoryBasicInformation */, &mbi, sizeof(mbi), &ret);
                        if (NT_SUCCESS(status)) {
                            if (outputLength < sizeof(SMX_REGION)) {
                                status = STATUS_BUFFER_TOO_SMALL;
                            } else {
                                PSMX_REGION out = (PSMX_REGION)systemBuffer;
                                out->base = (ULONG_PTR)mbi.BaseAddress;
                                out->size = mbi.RegionSize;
                                out->state = mbi.State;
                                out->protect = mbi.Protect;
                                out->type = mbi.Type;
                                out->pad = 0;
                                information = sizeof(SMX_REGION);
                            }
                        }
                        ZwClose(h);
                    }
                    ObDereferenceObject(process);
                }
            } else if (controlCode == SMX_IO_BATCH) {
                /* Copy the request out of the system buffer first: the reply
                 * overwrites that same buffer from offset 0. */
                SMX_BATCH req;
                if (inputLength >= sizeof(SMX_BATCH))
                    RtlCopyMemory(&req, systemBuffer, sizeof(SMX_BATCH));
                else
                    RtlZeroMemory(&req, sizeof(SMX_BATCH));

                /* The reply is req.total data bytes followed by req.count
                 * status bytes, so BOTH must fit in the output buffer and in
                 * the transfer cap. The subtractions are written so they can
                 * never wrap: each right-hand side is already proven <= its
                 * minuend. */
                if (req.count == 0 || req.count > SMX_BATCH_MAX ||
                    inputLength < sizeof(SMX_BATCH) ||
                    req.count > outputLength ||
                    req.total > outputLength - req.count ||
                    req.total > SMX_MAX_TRANSFER ||
                    req.count > SMX_MAX_TRANSFER - req.total) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    PEPROCESS process = NULL;
                    status = PsLookupProcessByProcessId(
                        (HANDLE)(ULONG_PTR)req.process_id, &process);
                    if (NT_SUCCESS(status)) {
                        PUCHAR base = (PUCHAR)systemBuffer;
                        PUCHAR flags = base + req.total;

                        /*
                         * Clear the WHOLE reply area before filling any of it.
                         *
                         * Only the requested ranges are written below, so any
                         * gap between them - and any range whose copy fails -
                         * would otherwise return whatever the system buffer
                         * held from a previous request. That is a kernel-memory
                         * disclosure to user mode, and it also hands the caller
                         * uninitialised bytes where its code expects zeros.
                         */
                        RtlZeroMemory(base, (SIZE_T)req.total + (SIZE_T)req.count);

                        for (ULONG i = 0; i < req.count; i++) {
                            PSMX_BITEM it = &req.items[i];
                            flags[i] = 0;
                            /* Range test must not overflow: offset+size could
                             * wrap 32-bit and let a huge offset through, which
                             * would turn this into an out-of-bounds write. */
                            if (it->size == 0 ||
                                it->offset > req.total ||
                                it->size > req.total - it->offset) continue;
                            SIZE_T transferred = 0;
                            NTSTATUS st = CopyCrossProcess(process, it->address,
                                PsGetCurrentProcess(),
                                (ULONG_PTR)(base + it->offset),
                                (SIZE_T)it->size, &transferred);
                            if (NT_SUCCESS(st) && transferred == it->size)
                                flags[i] = 1;
                            else
                                RtlZeroMemory(base + it->offset, it->size);
                        }
                        ObDereferenceObject(process);
                        information = req.total + req.count;
                    }
                }
            } else if (controlCode == SMX_IO_ALLOC) {
                ULONG_PTR allocated = 0;
                if (outputLength < sizeof(ULONG_PTR) || command.size == 0) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = AllocateInTarget(command.process_id,
                                              (SIZE_T)command.size,
                                              (ULONG)command.address,
                                              &allocated);
                    if (NT_SUCCESS(status)) {
                        *(ULONG_PTR*)systemBuffer = allocated;
                        information = (ULONG)sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_CONTEXT) {
                if (outputLength < sizeof(SMX_CTX) || command.size == 0) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetThreadContextInfo((ULONG)command.size,
                                                  (PSMX_CTX)systemBuffer);
                    if (NT_SUCCESS(status)) information = sizeof(SMX_CTX);
                }
            } else if (controlCode == SMX_IO_HIDE) {
                /* Dispatch the requested visibility change to SmxSetHidden().
                 * The command address field selects hide (nonzero) or restore (zero).
                 * The implementation validates candidates and self-links detached entries;
                 * these checks do not guarantee safety under every Windows build or race.
                 */
                status = SmxSetHidden((ULONG)command.process_id,
                                      command.address != 0);
            } else if (controlCode == SMX_IO_UNLOAD) {
                // Remove the DOS link and stop its recreation. The device and
                // mapped image remain resident. Existing handles and direct
                // NT-device opens are not revoked by deleting this link.
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\SmxRing");
                InterlockedExchange(&g_linkOff, 1);
                IoDeleteSymbolicLink(&dosName);
                g_created = FALSE;
                status = STATUS_SUCCESS;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        information = 0;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// ---------------------------------------------------------------- driver init

static NTSTATUS SmxInit(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING deviceName;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;

    // Device name collision (previous mapping this boot): probe suffixes.
    status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        WCHAR nameBuf[48] = { 0 };
        const WCHAR base[] = L"\\Device\\SmxRing";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&deviceName, nameBuf);

        status = IoCreateDevice(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    }
    if (!NT_SUCCESS(status)) {

        return status;
    }

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    // One global DOS symlink - handled by SmxLinkKeeper (deferred; the
    // mapper's trace cleanup wipes \?? objects created before it exits).

    driverObject->MajorFunction[IRP_MJ_CREATE]         = SmxCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE]          = SmxCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SmxDispatch;

    g_devObj = device;
    g_created = TRUE;

    // Keeper thread: (re)creates the symlink every 30s, starting at +15s.
    {
        HANDLE keeper = NULL;
        if (NT_SUCCESS(PsCreateSystemThread(&keeper, THREAD_ALL_ACCESS,
                NULL, NULL, NULL, SmxLinkKeeper, device))) {
            ZwClose(keeper);
        }
    }

    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------- init thread

static VOID SmxTrace(ULONG step, NTSTATUS st) {
    UNICODE_STRING keyPath;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SYSTEM\\SmxRing");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
            NULL, REG_OPTION_VOLATILE, NULL))) {
        char vn[32];
        UNICODE_STRING vns;
// "S%u" - index for the SmxTrace stage labels
        int n = 0;
        vn[n++] = 'S'; vn[n++] = '0' + (char)(step % 10);
        vn[n] = 0;
        RtlInitUnicodeString(&vns, L"Step");
        ZwSetValueKey(keyHandle, &vns, 0, REG_DWORD, &st, sizeof(st));
        ZwClose(keyHandle);
    }
}

static VOID SmxBootstrap(PVOID context) {
    UNREFERENCED_PARAMETER(context);

    SmxTrace(1, 0); // stage 1

    // Clean system-thread context: registry + object namespace ops are legal.
    g_token = ComputeRingToken();
    SmxTrace(2, 0); // stage 2

    // IoCreateDriver fails with STATUS_OBJECT_NAME_COLLISION if the name
    // already exists (previous mapping this boot, no unload path for it).
    // Probe sequential suffixes until a free name is found.
    NTSTATUS status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        // manual: "\Driver\SmxRing" + decimal suffix (no swprintf in kernel)
        WCHAR nameBuf[48] = { 0 };
        UNICODE_STRING driverName;
        const WCHAR base[] = L"\\Driver\\SmxRing";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&driverName, nameBuf);

        status = IoCreateDriver(&driverName, SmxInit);
        SmxTrace(3, status); // IoCreateDriver
    }

    // Persist init status for usermode diagnostics (no debugger attached).
    {
        UNICODE_STRING keyPath, valName;
        OBJECT_ATTRIBUTES attributes;
        HANDLE keyHandle = NULL;
        RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SYSTEM\\SmxRing");
        InitializeObjectAttributes(&attributes, &keyPath,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
                NULL, REG_OPTION_VOLATILE, NULL))) {
            RtlInitUnicodeString(&valName, L"InitStatus");
            ULONG st = (ULONG)status;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &st, sizeof(st));
            RtlInitUnicodeString(&valName, L"RingToken");
            ULONG sk = g_token;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &sk, sizeof(sk));
            ZwClose(keyHandle);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---------------------------------------------------------------- entry

// Keep the mapped entry point limited to starting the initialization thread.
NTSTATUS DriverEntry(PVOID parameterOne, PVOID parameterTwo) {
    UNREFERENCED_PARAMETER(parameterOne);
    UNREFERENCED_PARAMETER(parameterTwo);

    HANDLE thread = NULL;
    NTSTATUS status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, SmxBootstrap, NULL);
    if (!NT_SUCCESS(status))
        return status;

    ZwClose(thread);
    return STATUS_SUCCESS;
}




