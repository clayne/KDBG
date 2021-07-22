#include "global.h"
#include "common.h"
#include "ioctrl.h"
#include "device.h"
#include "krnl.h"
#include "pe.h"
#include "thread.h"

// TODO: refactor ptr to stack objects

/*
* Global driver state.
*/

ULONG Pid = 0;

MODULE Modules[KMOD_MAX_MODULES] = {}; // Buffer is required in order to copy from process memory to kernel memory to process again.
THREAD Threads[KMOD_MAX_THREADS] = {}; // Buffer is required in order to copy from process memory to kernel memory to process again.

/*
* Stack frames.
*/

typedef struct _STACK_FRAME_X64
{
  ULONG64 AddrOffset;
  ULONG64 StackOffset;
  ULONG64 FrameOffset;
} STACK_FRAME_X64, * PSTACK_FRAME_X64;

/*
* Process utilities relative to kernel space.
*/

NTSTATUS CopyUserSpaceMemorySafe(PVOID dst, PVOID src, SIZE_T size, KPROCESSOR_MODE mode)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PMDL mdl = IoAllocateMdl(src, size, FALSE, FALSE, NULL);
  if (mdl)
  {
    MmProbeAndLockPages(mdl, mode, IoReadAccess);
    PVOID mappedSrc = MmMapLockedPagesSpecifyCache(mdl, mode, MmNonCached, NULL, FALSE, HighPagePriority);
    if (mappedSrc)
    {
      status = MmProtectMdlSystemAddress(mdl, PAGE_READONLY);
      if (NT_SUCCESS(status))
      {
        __try
        {
          memcpy(dst, mappedSrc, size);
          status = STATUS_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
          KMOD_LOG_INFO("Something went wrong\n");
        }
      }
      MmUnmapLockedPages(mappedSrc, mdl);
    }
    MmUnlockPages(mdl);
  }
  IoFreeMdl(mdl);
  return status;
}

NTSTATUS FetchProcessModules()
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PEPROCESS process = NULL;
  KAPC_STATE apc;
  status = PsLookupProcessByProcessId((HANDLE)Pid, &process);
  if (NT_SUCCESS(status))
  {
    status = STATUS_UNSUCCESSFUL;
    KeStackAttachProcess(process, &apc);
    __try
    {
      PPEB64 peb = (PPEB64)PsGetProcessPeb(process);
      if (peb)
      {
        memset(Modules, 0, sizeof(MODULE) * KMOD_MAX_MODULES);
        PVOID imageBase = peb->ImageBaseAddress;
        PLDR_DATA_TABLE_ENTRY modules = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);;
        PLDR_DATA_TABLE_ENTRY module = NULL;
        PLIST_ENTRY moduleHead = modules->InMemoryOrderLinks.Flink;
        PLIST_ENTRY moduleEntry = moduleHead->Flink;
        ULONG count = 0;
        while (moduleEntry != moduleHead)
        {
          module = CONTAINING_RECORD(moduleEntry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
          if (module && module->DllBase)
          {
            CopyUserSpaceMemorySafe(&Modules[count].Base, &module->DllBase, sizeof(ULONG64), KernelMode);
            CopyUserSpaceMemorySafe(&Modules[count].Name, module->BaseDllName.Buffer, sizeof(WCHAR) * module->BaseDllName.Length, KernelMode);
            CopyUserSpaceMemorySafe(&Modules[count].Size, &module->SizeOfImage, sizeof(ULONG), KernelMode);
            count++;
            if (count >= KMOD_MAX_MODULES)
            {
              break;
            }
          }
          moduleEntry = moduleEntry->Flink;
        }
        KMOD_LOG_INFO("Fetched modules\n");
        status = STATUS_SUCCESS;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      KMOD_LOG_ERROR("Something went wrong!\n");
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(process);
  }
  return status;
}
NTSTATUS FetchProcessThreads()
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  ULONG read = 0;
  PBYTE buffer = (PBYTE)RtlAllocateMemory(TRUE, 1024 * 1024);
  status = ZwQuerySystemInformation(SystemProcessInformation, buffer, read, &read);
  ULONG processAcc = 0;
  while (1)
  {
    KMOD_LOG_INFO("Thread count %u\n", ((PSYSTEM_PROCESS_INFORMATION)buffer)->NumberOfThreads);
    //KMOD_LOG_INFO("Tid %u Pid %u\n", (ULONG)threads[i].ClientId.UniqueThread, (ULONG)threads[i].ClientId.UniqueProcess);
    //KMOD_LOG_INFO("Copy from %p to %p\n", &threads[i].ClientId.UniqueThread, &Threads[i].Tid);
    //for (ULONG i = 0; i < processInfo->NumberOfThreads; ++i)
    //{
    //  PSYSTEM_THREAD_INFORMATION thread = (PSYSTEM_THREAD_INFORMATION)(((PBYTE)processInfo) + sizeof(SYSTEM_PROCESS_INFORMATION) + sizeof(SYSTEM_THREAD_INFORMATION) * i);
    //  if (verbose)
    //  {
    //    LOG_INFO("\tTid: %u\n", *(PULONG)thread->ClientId.UniqueThread);
    //    LOG_INFO("\tBase: %p\n", thread->StartAddress);
    //    LOG_INFO("\n");
    //  }
    //  //request->Processes[processAcc].Threads[i].Tid = *(PULONG)thread->ClientId.UniqueThread;
    //  //request->Processes[processAcc].Threads[i].Base = thread->StartAddress;
    //  //request->Processes[processAcc].Threads[i].State = thread->ThreadState;
    //}
    if (!((PSYSTEM_PROCESS_INFORMATION)buffer)->NextEntryOffset)
    {
      KMOD_LOG_INFO("Fetched threads\n");
      status = STATUS_SUCCESS;
      break;
    }
    buffer += ((PSYSTEM_PROCESS_INFORMATION)buffer)->NextEntryOffset;
    processAcc++;
  }
  RtlFreeMemory(buffer);
  return status;
}
NTSTATUS GetProcessModules(ULONG pid, SIZE_T size, SIZE_T& count, PVOID buffer)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PEPROCESS process = NULL;
  KAPC_STATE apc;
  status = PsLookupProcessByProcessId((HANDLE)pid, &process);
  if (NT_SUCCESS(status))
  {
    status = STATUS_UNSUCCESSFUL;
    KeStackAttachProcess(process, &apc);
    KMOD_LOG_INFO("Attached\n");
    __try
    {
      PPEB64 peb = (PPEB64)PsGetProcessPeb(process);
      if (peb)
      {
        KMOD_LOG_INFO("Found PEB\n");
        PVOID imageBase = peb->ImageBaseAddress;
        PLDR_DATA_TABLE_ENTRY modules = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);;
        PLDR_DATA_TABLE_ENTRY module = NULL;
        PLIST_ENTRY moduleHead = modules->InMemoryOrderLinks.Flink;
        PLIST_ENTRY moduleEntry = moduleHead->Flink;
        while (moduleEntry != moduleHead)
        {
          module = CONTAINING_RECORD(moduleEntry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
          KMOD_LOG_INFO("%ls\n", module->BaseDllName.Buffer);
          if (module && module->DllBase)
          {
            //KMOD_LOG_INFO("Copy from %p to %p\n", &module->DllBase, &((PMODULE)buffer)[count].Base);
            //((PMODULE)buffer)[count].Base = (ULONG64)module->DllBase;
            //wcscpy(((PMODULE)buffer)[count].Name, module->BaseDllName.Buffer);
            //((PMODULE)buffer)[count].Size = module->SizeOfImage;
            CopyUserSpaceMemorySafe(&((PMODULE)buffer)[count].Base, &module->DllBase, sizeof(ULONG64), UserMode);
            CopyUserSpaceMemorySafe(&((PMODULE)buffer)[count].Size, &module->DllBase, sizeof(SIZE_T), UserMode);
            count++;
            KMOD_LOG_INFO("%llu copied %ls\n", count, module->BaseDllName.Buffer);
            KMOD_LOG_INFO("value is %p\n");
            if (count >= size)
            {
              break;
            }
          }
          moduleEntry = moduleEntry->Flink;
        }
        status = STATUS_SUCCESS;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      KMOD_LOG_ERROR("Something went wrong!\n");
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(process);
  }
  return status;
}
NTSTATUS GetProcessModuleBase(ULONG pid, PWCHAR name, PVOID& base)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PEPROCESS process = NULL;
  KAPC_STATE apc;
  status = PsLookupProcessByProcessId((HANDLE)pid, &process);
  if (NT_SUCCESS(status))
  {
    status = STATUS_UNSUCCESSFUL;
    KeStackAttachProcess(process, &apc);
    __try
    {
      PPEB64 peb = (PPEB64)PsGetProcessPeb(process);
      if (peb)
      {
        PVOID imageBase = peb->ImageBaseAddress;
        PLDR_DATA_TABLE_ENTRY modules = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);;
        PLDR_DATA_TABLE_ENTRY module = NULL;
        PLIST_ENTRY moduleHead = modules->InMemoryOrderLinks.Flink;
        PLIST_ENTRY moduleEntry = moduleHead->Flink;
        while (moduleEntry != moduleHead)
        {
          module = CONTAINING_RECORD(moduleEntry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
          if (module && module->DllBase)
          {
            if (_wcsicmp(name, module->BaseDllName.Buffer) == 0)
            {
              break;
            }
          }
          moduleEntry = moduleEntry->Flink;
        }
        base = module->DllBase;
        status = STATUS_SUCCESS;
        KMOD_LOG_INFO("Selected module %ls\n", module->BaseDllName.Buffer);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      KMOD_LOG_ERROR("Something went wrong!\n");
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(process);
  }
  return status;
}

NTSTATUS ReadVirtualProcessMemory(ULONG pid, PVOID base, SIZE_T size, PVOID buffer)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PEPROCESS process = NULL;
  KAPC_STATE apc;
  status = PsLookupProcessByProcessId((HANDLE)pid, &process);
  if (NT_SUCCESS(status))
  {
    status = STATUS_UNSUCCESSFUL;
    PBYTE asyncBuffer = (PBYTE)RtlAllocateMemory(TRUE, size);
    if (asyncBuffer)
    {
      PMDL mdl = IoAllocateMdl(base, size, FALSE, FALSE, NULL);
      if (mdl)
      {
        KeStackAttachProcess(process, &apc);
        __try
        {
          MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
          PBYTE mappedBuffer = (PBYTE)MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmNonCached, NULL, FALSE, HighPagePriority);
          if (mappedBuffer)
          {
            status = MmProtectMdlSystemAddress(mdl, PAGE_READONLY);
            if (NT_SUCCESS(status))
            {
              status = STATUS_UNSUCCESSFUL;
              memcpy(asyncBuffer, mappedBuffer, size);
              KMOD_LOG_INFO("Copy successfull\n");
              status = STATUS_SUCCESS;
            }
            MmUnmapLockedPages(mappedBuffer, mdl);
          }
          MmUnlockPages(mdl);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
          KMOD_LOG_ERROR("Something went wrong!\n");
          status = STATUS_UNHANDLED_EXCEPTION;
        }
        KeUnstackDetachProcess(&apc);
        IoFreeMdl(mdl);
      }
      memcpy(buffer, asyncBuffer, size);
      RtlFreeMemory(asyncBuffer);
    }
    ObDereferenceObject(process);
  }
  return status;
}
NTSTATUS WriteVirtualProcessMemory(ULONG pid, PVOID base, SIZE_T size, PVOID buffer)
{
  return STATUS_UNSUCCESSFUL;
}

/*
* Scanning utilities.
*/

VOID ScanContext(HANDLE tid, SIZE_T iterations)
{
  //NTSTATUS status = STATUS_SUCCESS;
  //PETHREAD thread = NULL;
  //PCONTEXT context = NULL;
  //SIZE_T contextSize = sizeof(CONTEXT);
  //status = PsLookupThreadByThreadId(tid, &thread);
  //KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "PsLookupThreadByThreadId %X\n", status);
  //status = ZwAllocateVirtualMemory(ZwCurrentProcess(), (PVOID*)&context, 0, &contextSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  //KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "ZwAllocateVirtualMemory %X\n", status);
  //RtlZeroMemory(context, contextSize);
  //LOG_INFO("Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi\n");
  //for (SIZE_T i = 0; i < iterations; ++i)
  //{
  //  context->ContextFlags = CONTEXT_ALL;
  //  status = PsGetContextThread(thread, context, UserMode);
  //  KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "PsGetContextThread %X\n", status);
  //  LOG_INFO("%llu %llu %llu %llu %llu %llu %llu %llu\n", context->Rax, context->Rcx, context->Rdx, context->Rbx, context->Rsp, context->Rbp, context->Rsi, context->Rdi);
  //}
  //if (context)
  //{
  //  ZwFreeVirtualMemory(ZwCurrentProcess(), (PVOID*)context, &contextSize, MEM_RELEASE);
  //}
  //ObDereferenceObject(thread);
}
VOID ScanStack(HANDLE pid, HANDLE tid, PWCHAR moduleName, SIZE_T iterations)
{
  //NTSTATUS status = STATUS_SUCCESS;
  //PEPROCESS process = NULL;
  //PETHREAD thread = NULL;
  //PCONTEXT context = NULL;
  //SIZE_T contextSize = sizeof(CONTEXT);
  //PLDR_DATA_TABLE_ENTRY modules = NULL;
  //PLDR_DATA_TABLE_ENTRY module = NULL;
  //PLIST_ENTRY moduleList = NULL;
  //PLIST_ENTRY moduleEntry = NULL;
  //STACK_FRAME_X64 stackFrame64;
  //PPEB64 peb = NULL;
  //KAPC_STATE apc;
  //// Get context infos
  //status = PsLookupProcessByProcessId(pid, &process);
  //KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "PsLookupProcessByProcessId %X", status);
  //status = PsLookupThreadByThreadId(tid, &thread);
  //KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "PsLookupThreadByThreadId %X\n", status);
  //status = ZwAllocateVirtualMemory(ZwCurrentProcess(), (PVOID*)&context, 0, &contextSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  //KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "ZwAllocateVirtualMemory %X\n", status);
  //// Attach to process
  //KeStackAttachProcess(process, &apc);
  //// Get module base
  //peb = (PPEB64)PsGetProcessPeb(process);
  //KMOD_LOG_ERROR_IF_NOT(!peb, "PsGetProcessPeb\n");
  //modules = CONTAINING_RECORD(peb->Ldr->InLoadOrderModuleList.Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
  //moduleList = modules->InLoadOrderLinks.Flink;
  //moduleEntry = moduleList->Flink;
  //while (moduleEntry != moduleList)
  //{
  //  module = CONTAINING_RECORD(moduleEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
  //  if (wcscmp(moduleName, module->BaseDllName.Buffer) == 0)
  //  {
  //    break;
  //  }
  //  moduleEntry = moduleEntry->Flink;
  //}
  //KMOD_LOG_ERROR_IF_NOT(!module, "Module not found\n");
  //PVOID moduleBase = GetModuleBase((PBYTE)module->DllBase, *(PULONG)module->EntryPoint);
  //LOG_INFO("ModuleBase %p\n", moduleBase);
  ////ULONG moduleExportOffset = GetModuleExportOffset((PBYTE)module->DllBase, module->SizeOfImage, "");
  ////LOG_INFO("moduleExportOffset %p\n", moduleExportOffset);
  //// Dump exports
  //DumpModuleExports((PBYTE)module->DllBase, module->SizeOfImage);
  //// Dump stack frames
  //LOG_INFO("\n");
  //LOG_INFO("Stack Frames:\n");
  //for (SIZE_T i = 0; i < iterations; ++i)
  //{
  //  // Get context
  //  status = PsGetContextThread(thread, context, UserMode);
  //  KMOD_LOG_ERROR_IF_NOT_SUCCESS(status, "PsGetContextThread %X\n", status);
  //  // Get stack frame
  //  stackFrame64.AddrOffset = context->Rip; // Instruction ptr
  //  stackFrame64.StackOffset = context->Rsp; // Stack ptr
  //  stackFrame64.FrameOffset = context->Rbp; // Stack base ptr
  //  LOG_INFO("%4X %llu %llu %llu\n", i, stackFrame64.AddrOffset, stackFrame64.StackOffset, stackFrame64.FrameOffset);
  //}
  //// Detach from process
  //KeUnstackDetachProcess(&apc);
  //// Cleanup
  //if (context)
  //{
  //  ZwFreeVirtualMemory(ZwCurrentProcess(), (PVOID*)context, &contextSize, MEM_RELEASE);
  //}
  //ObDereferenceObject(thread);
  //ObDereferenceObject(process);
}

/*
* Communication device.
*/

PDEVICE_OBJECT Device = NULL;

#define KMOD_DEVICE_NAME L"\\Device\\KMOD"
#define KMOD_DEVICE_SYMBOL_NAME L"\\DosDevices\\KMOD"

/*
* Request/Response handlers.
*/

NTSTATUS HandleProcessAttachRequest(PREQ_PROCESS_ATTACH req)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  Pid = req->In.Pid;
  KMOD_LOG_INFO("Attached to process %u\n", Pid);
  status = STATUS_SUCCESS;
  return status;
}
NTSTATUS HandleProcessModulesRequest(PREQ_PROCESS_MODULES req)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  if (Pid)
  {
    status = FetchProcessModules();
    if (NT_SUCCESS(status))
    {
      memcpy(req->Out.Buffer, Modules, sizeof(MODULE) * KMOD_MAX_MODULES);
    }
  }
  return status;
}
NTSTATUS HandleProcessThreadsRequest(PREQ_PROCESS_THREADS req)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  if (Pid)
  {
    status = FetchProcessThreads();
    if (NT_SUCCESS(status))
    {
      memcpy(req->Out.Buffer, Threads, sizeof(THREAD) * KMOD_MAX_THREADS);
    }
  }
  return status;
}
NTSTATUS HandleMemoryReadRequest(PREQ_MEMORY_READ req)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PVOID base = NULL;
  if (Pid)
  {
    status = GetProcessModuleBase(Pid, req->In.Name, base);
    if (NT_SUCCESS(status))
    {
      req->Out.Base = (ULONG64)base;
      status = ReadVirtualProcessMemory(Pid, (PVOID)((PBYTE)base + req->In.Offset), req->In.Size, req->Out.Buffer);
    }
  }
  return status;
}
NTSTATUS HandleMemoryWriteRequest(PREQ_MEMORY_WRITE req)
{
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  PVOID base = NULL;
  if (Pid)
  {
    status = GetProcessModuleBase(Pid, req->In.Name, base);
    if (NT_SUCCESS(status))
    {
      req->Out.Base = (ULONG64)base;
      status = WriteVirtualProcessMemory(Pid, (PVOID)((PBYTE)base + req->In.Offset), req->In.Size, req->Out.Buffer);
    }
  }
  return status;
}

/*
* I/O callbacks.
*/

NTSTATUS OnIrpDflt(PDEVICE_OBJECT device, PIRP irp)
{
  UNREFERENCED_PARAMETER(device);
  irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
  irp->IoStatus.Information = 0;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return irp->IoStatus.Status;
}
NTSTATUS OnIrpCreate(PDEVICE_OBJECT device, PIRP irp)
{
  UNREFERENCED_PARAMETER(device);
  irp->IoStatus.Status = STATUS_SUCCESS;
  irp->IoStatus.Information = 0;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return irp->IoStatus.Status;
}
NTSTATUS OnIrpCtrl(PDEVICE_OBJECT device, PIRP irp)
{
  UNREFERENCED_PARAMETER(device);
  KMOD_LOG_INFO("========================================\n");
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  switch (stack->Parameters.DeviceIoControl.IoControlCode)
  {
    case KMOD_REQ_PROCESS_ATTACH:
    {
      KMOD_LOG_INFO("Begin process attach\n");
      PREQ_PROCESS_ATTACH req = (PREQ_PROCESS_ATTACH)irp->AssociatedIrp.SystemBuffer;
      irp->IoStatus.Status = HandleProcessAttachRequest(req);
      irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? sizeof(REQ_PROCESS_ATTACH) : 0;
      KMOD_LOG_INFO("End process attach\n");
      break;
    }
    case KMOD_REQ_PROCESS_MODULES:
    {
      KMOD_LOG_INFO("Begin process modules\n");
      PREQ_PROCESS_MODULES req = (PREQ_PROCESS_MODULES)irp->AssociatedIrp.SystemBuffer;
      irp->IoStatus.Status = HandleProcessModulesRequest(req);
      irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? sizeof(REQ_PROCESS_MODULES) : 0;
      KMOD_LOG_INFO("End process modules\n");
      break;
    }
    case KMOD_REQ_PROCESS_THREADS:
    {
      KMOD_LOG_INFO("Begin process threads\n");
      PREQ_PROCESS_THREADS req = (PREQ_PROCESS_THREADS)irp->AssociatedIrp.SystemBuffer;
      irp->IoStatus.Status = HandleProcessThreadsRequest(req);
      irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? sizeof(REQ_PROCESS_THREADS) : 0;
      KMOD_LOG_INFO("End process threads\n");
      break;
    }
    case KMOD_REQ_MEMORY_READ:
    {
      KMOD_LOG_INFO("Begin memory read\n");
      PREQ_MEMORY_READ req = (PREQ_MEMORY_READ)irp->AssociatedIrp.SystemBuffer;
      irp->IoStatus.Status = HandleMemoryReadRequest(req);
      irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? sizeof(REQ_MEMORY_READ) : 0;
      KMOD_LOG_INFO("End memory read\n");
      break;
    }
    case KMOD_REQ_MEMORY_WRITE:
    {
      KMOD_LOG_INFO("Begin memory write\n");
      PREQ_MEMORY_WRITE req = (PREQ_MEMORY_WRITE)irp->AssociatedIrp.SystemBuffer;
      irp->IoStatus.Status = HandleMemoryWriteRequest(req);
      irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? sizeof(REQ_MEMORY_WRITE) : 0;
      KMOD_LOG_INFO("End memory write\n");
      break;
    }
  }
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  KMOD_LOG_INFO("========================================\n");
  return irp->IoStatus.Status;
}
NTSTATUS OnIrpClose(PDEVICE_OBJECT device, PIRP irp)
{
  UNREFERENCED_PARAMETER(device);
  irp->IoStatus.Status = STATUS_SUCCESS;
  irp->IoStatus.Information = 0;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return irp->IoStatus.Status;
}

/*
* Entry point.
*/

VOID DriverUnload(PDRIVER_OBJECT driver)
{
  UNREFERENCED_PARAMETER(driver);
  NTSTATUS status = STATUS_SUCCESS;
  DeleteDevice(Device, KMOD_DEVICE_SYMBOL_NAME);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING regPath)
{
  UNREFERENCED_PARAMETER(regPath);
  NTSTATUS status = STATUS_SUCCESS;
  driver->DriverUnload = DriverUnload;
  for (ULONG i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i)
    driver->MajorFunction[i] = OnIrpDflt;
  driver->MajorFunction[IRP_MJ_CREATE] = OnIrpCreate;
  driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = OnIrpCtrl;
  driver->MajorFunction[IRP_MJ_CLOSE] = OnIrpClose;
  CreateDevice(driver, Device, KMOD_DEVICE_NAME, KMOD_DEVICE_SYMBOL_NAME);
  return status;
}