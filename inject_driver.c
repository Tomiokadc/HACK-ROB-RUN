#include <ntifs.h>
#include <wdmsec.h>

#include "protocol.h"

#define INJECT_TAG '0tjI'

/* Declared manually: the WDK headers used here do not expose all of them. */
NTKERNELAPI NTSTATUS NTAPI IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction
);
NTKERNELAPI NTSTATUS NTAPI PsAcquireProcessExitSynchronization(PEPROCESS Process);
NTKERNELAPI VOID NTAPI PsReleaseProcessExitSynchronization(PEPROCESS Process);
NTKERNELAPI ULONG NTAPI PsGetProcessSessionId(PEPROCESS Process);
NTKERNELAPI PPEB NTAPI PsGetProcessPeb(PEPROCESS Process);
NTKERNELAPI PCHAR NTAPI PsGetProcessImageFileName(PEPROCESS Process);
C_ASSERT(sizeof(INJECT_BIND_REQUEST) == 8);
C_ASSERT(sizeof(INJECT_INFO_RESPONSE) == 32);
C_ASSERT(sizeof(INJECT_ALLOC_REQUEST) == 16);
C_ASSERT(sizeof(INJECT_FREE_REQUEST) == 16);
C_ASSERT(sizeof(INJECT_WRITE_REQUEST) == 16);
C_ASSERT(sizeof(INJECT_READ_REQUEST) == 16);
C_ASSERT(sizeof(INJECT_PROTECT_REQUEST) == 24);
C_ASSERT(sizeof(INJECT_APC_REQUEST) == 48);
C_ASSERT(sizeof(INJECT_MODULE_REQUEST) == 136);
C_ASSERT(sizeof(INJECT_MODULE_RESPONSE) == 16);
C_ASSERT(sizeof(INJECT_MODULE_ENUM_RESPONSE) == 144);

typedef struct _INJECT_CLIENT {
    PEPROCESS Process;
    PEPROCESS Target;
    FAST_MUTEX Lock;
} INJECT_CLIENT;


/* Layout of the fields used from LDR_DATA_TABLE_ENTRY on x64. */
typedef struct _INJECT_LDR_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;
    PVOID          ExceptionTable;
    ULONG          ExceptionTableSize;
    ULONG          Reserved0;
    PVOID          GpValue;
    PVOID          NonPagedDebugInfo;
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    ULONG          Reserved1;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} INJECT_LDR_ENTRY, *PINJECT_LDR_ENTRY;

typedef struct _INJECT_PEB_LDR_DATA {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} INJECT_PEB_LDR_DATA, *PINJECT_PEB_LDR_DATA;

/* Only the Ldr pointer is used; on x64 it sits at PEB+0x18. */
typedef struct _INJECT_PEB {
    UCHAR Reserved0[0x18];
    PINJECT_PEB_LDR_DATA Ldr;
} INJECT_PEB, *PINJECT_PEB;



static const GUID g_DeviceClass =
    {0x5b1f6a2e, 0x8a44, 0x4d0b, {0x9c, 0x2f, 0x71, 0xb5, 0x3e, 0x04, 0x11, 0xc7}};

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN ImageNameIs(PCHAR Image, const char *Expected) {
    ULONG i = 0;
    if (Image == NULL) return FALSE;
    for (;;) {
        if (Image[i] != Expected[i]) return FALSE;
        if (Expected[i] == 0) return TRUE;
        i++;
    }
}

static BOOLEAN UserRange(ULONGLONG Address, ULONGLONG Size) {
    ULONGLONG Highest = (ULONGLONG)(ULONG_PTR)MmHighestUserAddress;
    if (Address < 0x10000 || Address > Highest) return FALSE;
    if (Size == 0) return FALSE;
    return Size <= Highest - Address + 1;
}

static INJECT_CLIENT *ClientOf(PIRP Irp) {
    PFILE_OBJECT File = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    return File != NULL ? (INJECT_CLIENT *)File->FsContext : NULL;
}

static PEPROCESS AcquireTarget(PIRP Irp) {
    INJECT_CLIENT *Client = ClientOf(Irp);
    PEPROCESS Process;
    if (Client == NULL) return NULL;
    ExAcquireFastMutex(&Client->Lock);
    Process = Client->Target;
    if (Process != NULL) ObReferenceObject(Process);
    ExReleaseFastMutex(&Client->Lock);
    return Process;
}

/* Handlers only prepare the result; Control is the single IRP completion owner. */
static NTSTATUS ControlResult(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    return Status;
}

static SIZE_T WideLength(PCWSTR Text) {
    SIZE_T i = 0;
    while (Text[i] != 0) i++;
    return i;
}

static BOOLEAN UnicodeEqualsCI(PCUNICODE_STRING Value, PCWSTR Text) {
    USHORT Length;
    USHORT i;
    if (Value == NULL || Value->Buffer == NULL) return FALSE;
    Length = (USHORT)(WideLength(Text) * sizeof(WCHAR));
    if (Value->Length != Length) return FALSE;
    for (i = 0; i < Value->Length / sizeof(WCHAR); i++) {
        if (RtlUpcaseUnicodeChar(Value->Buffer[i]) != RtlUpcaseUnicodeChar(Text[i])) return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------- BIND -- */

static NTSTATUS DoBind(PIRP Irp, PINJECT_BIND_REQUEST Request, ULONG InputLength) {
    PEPROCESS Process = NULL;
    INJECT_CLIENT *Client = ClientOf(Irp);
    NTSTATUS Status;

    if (InputLength < sizeof(*Request) || Request == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (Request->Version != INJECT_PROTOCOL_VERSION || Request->ProcessId == 0) return STATUS_INVALID_PARAMETER;

    Status = PsLookupProcessByProcessId(ULongToHandle(Request->ProcessId), &Process);
    if (!NT_SUCCESS(Status)) return Status;

    if (Process == PsGetCurrentProcess()) {
        ObDereferenceObject(Process);
        return STATUS_ACCESS_DENIED;
    }
    if (!ImageNameIs(PsGetProcessImageFileName(Process), INJECT_TARGET_IMAGE)) {
        ObDereferenceObject(Process);
        return STATUS_ACCESS_DENIED;
    }
    if (PsGetProcessSessionId(Process) != PsGetProcessSessionId(IoGetRequestorProcess(Irp))) {
        ObDereferenceObject(Process);
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireFastMutex(&Client->Lock);
    /* Binding is immutable for this handle: no request can switch targets mid-operation. */
    if (Client->Target != NULL) {
        Status = Client->Target == Process ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
        ExReleaseFastMutex(&Client->Lock);
        ObDereferenceObject(Process);
        return Status;
    }
    Client->Target = Process;
    ExReleaseFastMutex(&Client->Lock);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- INFO -- */

static NTSTATUS DoInfo(PIRP Irp, PVOID Buffer, ULONG InputLength, ULONG OutputLength) {
    INJECT_INFO_RESPONSE *Response = (INJECT_INFO_RESPONSE *)Buffer;
    PEPROCESS Process;
    UNREFERENCED_PARAMETER(InputLength);

    if (Response == NULL || OutputLength < sizeof(*Response)) return STATUS_BUFFER_TOO_SMALL;
    Process = AcquireTarget(Irp);
    RtlZeroMemory(Response, sizeof(*Response));
    Response->Version = INJECT_PROTOCOL_VERSION;
    Response->Features = INJECT_FEATURE_MODULES;
    if (Process != NULL) {
        Response->ProcessId = (ULONGLONG)(ULONG_PTR)PsGetProcessId(Process);
        Response->Peb = (ULONGLONG)(ULONG_PTR)PsGetProcessPeb(Process);
        ObDereferenceObject(Process);
    }
    return ControlResult(Irp, STATUS_SUCCESS, sizeof(*Response));
}

/* --------------------------------------------------------------- ALLOC -- */

/* ---------------------------------------------------------------- FREE -- */

/* --------------------------------------------------------------- WRITE -- */

/* ---------------------------------------------------------------- READ -- */

static NTSTATUS DoRead(PIRP Irp, PINJECT_READ_REQUEST Request, ULONG InputLength,
                       PVOID Buffer, ULONG OutputLength) {
    PEPROCESS Process;
    KAPC_STATE State;
    MM_COPY_ADDRESS Source;
    SIZE_T Copied = 0;
    ULONG Size;
    NTSTATUS Status;

    if (InputLength < sizeof(*Request) || Request == NULL || Buffer == NULL)
        return STATUS_BUFFER_TOO_SMALL;
    if (Request->Version != INJECT_PROTOCOL_VERSION) return STATUS_INVALID_PARAMETER;
    if (Request->Size == 0 || Request->Size > INJECT_MAX_TRANSFER) return STATUS_INVALID_PARAMETER;
    if (OutputLength < Request->Size) return STATUS_BUFFER_TOO_SMALL;
    if (!UserRange(Request->Address, Request->Size)) return STATUS_INVALID_PARAMETER;

    Size = Request->Size;
    Source.VirtualAddress = (PVOID)(ULONG_PTR)Request->Address;

    Process = AcquireTarget(Irp);
    if (Process == NULL) return STATUS_INVALID_DEVICE_STATE;

    Status = PsAcquireProcessExitSynchronization(Process);
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Process);
        return Status;
    }
    /* METHOD_BUFFERED supplies a nonpaged system buffer. The source can disappear
       after enumeration, so use a copy routine that reports inaccessible pages. */
    KeStackAttachProcess((PRKPROCESS)Process, &State);
    Status = MmCopyMemory(Buffer, Source, Size, MM_COPY_MEMORY_VIRTUAL, &Copied);
    KeUnstackDetachProcess(&State);
    if (NT_SUCCESS(Status) && Copied != Size) {
        Status = STATUS_PARTIAL_COPY;
    }
    PsReleaseProcessExitSynchronization(Process);
    ObDereferenceObject(Process);
    return ControlResult(Irp, Status, NT_SUCCESS(Status) ? Size : 0);
}

/* ------------------------------------------------------------- PROTECT -- */

/* ----------------------------------------------------------------- APC -- */

/* -------------------------------------------------------------- MODULE -- */

static NTSTATUS DoModule(PIRP Irp, PINJECT_MODULE_REQUEST Request, ULONG InputLength,
                         PVOID Buffer, ULONG OutputLength) {
    INJECT_MODULE_RESPONSE *Response = (INJECT_MODULE_RESPONSE *)Buffer;
    PEPROCESS Process;
    PINJECT_PEB Peb;
    KAPC_STATE State;
    NTSTATUS Status = STATUS_NOT_FOUND;

    if (InputLength < sizeof(*Request) || Request == NULL || Response == NULL ||
        OutputLength < sizeof(*Response))
        return STATUS_BUFFER_TOO_SMALL;
    if (Request->Version != INJECT_PROTOCOL_VERSION) return STATUS_INVALID_PARAMETER;
    if (Request->Name[INJECT_MAX_NAME - 1] != 0) return STATUS_INVALID_PARAMETER;

    Process = AcquireTarget(Irp);
    if (Process == NULL) return STATUS_INVALID_DEVICE_STATE;
    Peb = (PINJECT_PEB)PsGetProcessPeb(Process);
    if (Peb == NULL) {
        ObDereferenceObject(Process);
        return STATUS_NOT_FOUND;
    }

    Status = PsAcquireProcessExitSynchronization(Process);
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Process);
        return Status;
    }

    Status = STATUS_NOT_FOUND;
    __try {
        KeStackAttachProcess((PRKPROCESS)Process, &State);
        __try {
            PINJECT_PEB_LDR_DATA Ldr;
            ULONG Visited = 0;
            ProbeForRead(Peb, sizeof(*Peb), 1);
            Ldr = Peb->Ldr;
            if (Ldr != NULL) {
                ProbeForRead(Ldr, sizeof(*Ldr), 1);
                PLIST_ENTRY Head = &Ldr->InLoadOrderModuleList;
                PLIST_ENTRY Link;
                for (Link = Head->Flink; Link != Head; ) {
                    INJECT_LDR_ENTRY Snapshot;
                    PINJECT_LDR_ENTRY Entry = &Snapshot;
                    if (++Visited > 4096) { Status = STATUS_DATA_ERROR; break; }
                    ProbeForRead(Link, sizeof(Snapshot), 1);
                    RtlCopyMemory(&Snapshot, Link, sizeof(Snapshot));
                    if ((Entry->BaseDllName.Length & 1) != 0 ||
                        Entry->BaseDllName.Length > Entry->BaseDllName.MaximumLength) {
                        Status = STATUS_DATA_ERROR; break;
                    }
                    if (Entry->BaseDllName.Length != 0) {
                        ProbeForRead(Entry->BaseDllName.Buffer, Entry->BaseDllName.Length, 1);
                    }
                    if (UnicodeEqualsCI(&Entry->BaseDllName, Request->Name)) {
                        Response->Base = (ULONGLONG)(ULONG_PTR)Entry->DllBase;
                        Response->Size = Entry->SizeOfImage;
                        Response->Reserved = 0;
                        Status = STATUS_SUCCESS;
                        break;
                    }
                    Link = Snapshot.InLoadOrderLinks.Flink;
                }
            }
        } __finally {
            KeUnstackDetachProcess(&State);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
    }

    PsReleaseProcessExitSynchronization(Process);
    ObDereferenceObject(Process);
    if (!NT_SUCCESS(Status)) return ControlResult(Irp, Status, 0);
    return ControlResult(Irp, STATUS_SUCCESS, sizeof(*Response));
}

static NTSTATUS DoModuleEnum(PIRP Irp, PINJECT_MODULE_ENUM_REQUEST Request, ULONG InputLength,
                             PVOID Buffer, ULONG OutputLength) {
    INJECT_MODULE_ENUM_RESPONSE *Response = (INJECT_MODULE_ENUM_RESPONSE *)Buffer;
    PEPROCESS Process;
    PINJECT_PEB Peb;
    KAPC_STATE State;
    NTSTATUS Status = STATUS_NOT_FOUND;
    ULONG Index = 0;
    ULONG Wanted;

    if (InputLength < sizeof(*Request) || Request == NULL || Response == NULL ||
        OutputLength < sizeof(*Response))
        return STATUS_BUFFER_TOO_SMALL;
    if (Request->Version != INJECT_PROTOCOL_VERSION) return STATUS_INVALID_PARAMETER;

    /* The response shares the buffered system buffer with the request, so the
     * requested index must be saved before the response is zeroed. */
    Wanted = Request->Index;

    Process = AcquireTarget(Irp);
    if (Process == NULL) return STATUS_INVALID_DEVICE_STATE;
    Peb = (PINJECT_PEB)PsGetProcessPeb(Process);
    if (Peb == NULL) {
        ObDereferenceObject(Process);
        return STATUS_NOT_FOUND;
    }

    RtlZeroMemory(Response, sizeof(*Response));
    Status = PsAcquireProcessExitSynchronization(Process);
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Process);
        return Status;
    }
    Status = STATUS_NOT_FOUND;
    __try {
        KeStackAttachProcess((PRKPROCESS)Process, &State);
        __try {
            PINJECT_PEB_LDR_DATA Ldr;
            ULONG Visited = 0;
            ProbeForRead(Peb, sizeof(*Peb), 1);
            Ldr = Peb->Ldr;
            if (Ldr != NULL) {
                ProbeForRead(Ldr, sizeof(*Ldr), 1);
                PLIST_ENTRY Head = &Ldr->InLoadOrderModuleList;
                PLIST_ENTRY Link;
                for (Link = Head->Flink; Link != Head; ) {
                    INJECT_LDR_ENTRY Snapshot;
                    PINJECT_LDR_ENTRY Entry = &Snapshot;
                    if (++Visited > 4096) { Status = STATUS_DATA_ERROR; break; }
                    ProbeForRead(Link, sizeof(Snapshot), 1);
                    RtlCopyMemory(&Snapshot, Link, sizeof(Snapshot));
                    if ((Entry->BaseDllName.Length & 1) != 0 ||
                        Entry->BaseDllName.Length > Entry->BaseDllName.MaximumLength) {
                        Status = STATUS_DATA_ERROR; break;
                    }
                    if (Entry->BaseDllName.Length != 0) {
                        ProbeForRead(Entry->BaseDllName.Buffer, Entry->BaseDllName.Length, 1);
                    }
                    if (Index == Wanted) {
                        USHORT Count = Entry->BaseDllName.Length / sizeof(WCHAR);
                        if (Count > INJECT_MAX_NAME - 1) Count = INJECT_MAX_NAME - 1;
                        Response->Base = (ULONGLONG)(ULONG_PTR)Entry->DllBase;
                        Response->Size = Entry->SizeOfImage;
                        if (Entry->BaseDllName.Buffer != NULL) {
                            RtlCopyMemory(Response->Name, Entry->BaseDllName.Buffer,
                                          Count * sizeof(WCHAR));
                        }
                        Response->Name[Count] = 0;
                        Status = STATUS_SUCCESS;
                        break;
                    }
                    Index++;
                    Link = Snapshot.InLoadOrderLinks.Flink;
                }
            }
        } __finally {
            KeUnstackDetachProcess(&State);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
    }

    PsReleaseProcessExitSynchronization(Process);
    ObDereferenceObject(Process);
    if (!NT_SUCCESS(Status)) return ControlResult(Irp, Status, 0);
    return ControlResult(Irp, STATUS_SUCCESS, sizeof(*Response));
}

/* ------------------------------------------------------------- DISPATCH -- */

static NTSTATUS Dispatch(PDEVICE_OBJECT Device, PIRP Irp) {
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    INJECT_CLIENT *Client = Stack->FileObject != NULL
                                ? (INJECT_CLIENT *)Stack->FileObject->FsContext
                                : NULL;
    UNREFERENCED_PARAMETER(Device);

    switch (Stack->MajorFunction) {
    case IRP_MJ_CREATE: {
        INJECT_CLIENT *Context;
        if (Stack->FileObject == NULL) return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
        Context = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Context), INJECT_TAG);
        if (Context == NULL) return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        RtlZeroMemory(Context, sizeof(*Context));
        ExInitializeFastMutex(&Context->Lock);
        Context->Process = PsGetCurrentProcess();
        ObReferenceObject(Context->Process);
        Stack->FileObject->FsContext = Context;
        return Complete(Irp, STATUS_SUCCESS, 0);
    }
    case IRP_MJ_CLEANUP:
        /* Keep references until CLOSE, after outstanding requests have drained. */
        return Complete(Irp, STATUS_SUCCESS, 0);
    case IRP_MJ_CLOSE:
        if (Client != NULL) {
            if (Client->Target != NULL) ObDereferenceObject(Client->Target);
            if (Client->Process != NULL) ObDereferenceObject(Client->Process);
            ExFreePoolWithTag(Client, INJECT_TAG);
        }
        if (Stack->FileObject != NULL) Stack->FileObject->FsContext = NULL;
        return Complete(Irp, STATUS_SUCCESS, 0);
    default:
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS Control(PDEVICE_OBJECT Device, PIRP Irp) {
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    INJECT_CLIENT *Client = Stack->FileObject != NULL
                                ? (INJECT_CLIENT *)Stack->FileObject->FsContext
                                : NULL;
    ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PEPROCESS Caller = IoGetRequestorProcess(Irp);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Device);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Irp->RequestorMode != UserMode ||
        Client == NULL || Caller == NULL || Client->Process != Caller)
        return Complete(Irp, STATUS_ACCESS_DENIED, 0);

    Irp->IoStatus.Information = 0;
    switch (Code) {
    case INJECT_IOCTL_BIND:
        Status = DoBind(Irp, (PINJECT_BIND_REQUEST)Buffer, InputLength);
        break;
    case INJECT_IOCTL_INFO:
        Status = DoInfo(Irp, Buffer, InputLength, OutputLength);
        break;
    case INJECT_IOCTL_READ:
        Status = DoRead(Irp, (PINJECT_READ_REQUEST)Buffer, InputLength, Buffer, OutputLength);
        break;
    case INJECT_IOCTL_MODULE:
        Status = DoModule(Irp, (PINJECT_MODULE_REQUEST)Buffer, InputLength, Buffer, OutputLength);
        break;
    case INJECT_IOCTL_MODULE_ENUM:
        Status = DoModuleEnum(Irp, (PINJECT_MODULE_ENUM_REQUEST)Buffer, InputLength,
                              Buffer, OutputLength);
        break;
    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    return Complete(Irp, Status, NT_SUCCESS(Status) ? Irp->IoStatus.Information : 0);
}

static VOID ProbeUnload(PDRIVER_OBJECT Driver) {
    UNICODE_STRING LinkName;
    RtlInitUnicodeString(&LinkName, INJECT_SYMLINK_NAME);
    IoDeleteSymbolicLink(&LinkName);
    IoDeleteDevice(Driver->DeviceObject);
}

static NTSTATUS InjectInitialize(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
    UNICODE_STRING DeviceName;
    UNICODE_STRING LinkName;
    UNICODE_STRING Sddl;
    PDEVICE_OBJECT Device = NULL;
    NTSTATUS Status;
    ULONG i;



    if (Driver == NULL) return STATUS_INVALID_PARAMETER;


    RtlInitUnicodeString(&DeviceName, INJECT_DEVICE_NAME);
    RtlInitUnicodeString(&LinkName, INJECT_SYMLINK_NAME);
    RtlInitUnicodeString(&Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    Status = IoCreateDeviceSecure(Driver, 0, &DeviceName, FILE_DEVICE_UNKNOWN,
                                  FILE_DEVICE_SECURE_OPEN, FALSE, &Sddl,
                                  &g_DeviceClass, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) Driver->MajorFunction[i] = Dispatch;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Control;
    Driver->DriverUnload = RegistryPath != NULL ? ProbeUnload : NULL;

    Device->Flags |= DO_BUFFERED_IO;
    Device->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoCreateSymbolicLink(&LinkName, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(Device);
        return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
    if (Driver != NULL) return InjectInitialize(Driver, RegistryPath);
    return IoCreateDriver(NULL, InjectInitialize);
}
