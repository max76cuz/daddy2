#include <ntddk.h>
#include <vhf.h>       // Virtual HID Framework header
#include "public.h"

#define DEVICE_NAME L"\\Device\\VirtualHIDMouse"
#define SYMBOLIC_NAME L"\\DosDevices\\VirtualHIDMouse"

// Global variables
PDEVICE_OBJECT g_DeviceObject = NULL;
VHFDEVICE g_VhfDevice = NULL;

// Virtual HID Mouse Report Descriptor (Relative Mouse)
UCHAR VirtualMouseReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute) ;3 button bits
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Constant) ;5 bit padding
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Variable, Relative) ;2 position bytes (X & Y)
    0xC0,              //   End Collection
    0xC0               // End Collection
};

// Forward declarations
DRIVER_UNLOAD DriverUnload;
NTSTATUS DriverCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS DriverDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS InitializeVirtualMouse(PDRIVER_OBJECT DriverObject);
VOID InjectRelativeMouseInput(LONG dx, LONG dy);

//
// DriverEntry: Initialize driver, create device & symbolic link, initialize VHF virtual mouse
//
extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;

    // Initialize virtual HID mouse device using VHF
    status = InitializeVirtualMouse(DriverObject);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to create virtual HID mouse device: 0x%X\n", status));
        return status;
    }

    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(DEVICE_NAME);
    status = IoCreateDevice(DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN, 0, FALSE, &g_DeviceObject);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to create device object: 0x%X\n", status));
        VhfDeleteDevice(g_VhfDevice);
        return status;
    }

    UNICODE_STRING symbolicLink = RTL_CONSTANT_STRING(SYMBOLIC_NAME);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to create symbolic link: 0x%X\n", status));
        IoDeleteDevice(g_DeviceObject);
        VhfDeleteDevice(g_VhfDevice);
        return status;
    }

    // Setup dispatch functions
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = DriverCreateClose;

    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    KdPrint(("Driver loaded successfully\n"));
    return STATUS_SUCCESS;
}

//
// DriverUnload: Cleanup virtual mouse, symbolic link, and device
//
VOID DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_VhfDevice) {
        VhfDeleteDevice(g_VhfDevice);
        g_VhfDevice = NULL;
    }

    if (g_DeviceObject) {
        UNICODE_STRING symbolicLink = RTL_CONSTANT_STRING(SYMBOLIC_NAME);
        IoDeleteSymbolicLink(&symbolicLink);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }

    KdPrint(("Driver unloaded\n"));
}

//
// Create and Close dispatch
//
NTSTATUS DriverCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

//
// Device IO Control handler for memory read and mouse input injection
//
NTSTATUS DriverDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (controlCode) {
    case IOCTL_READ_MEMORY: {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(READ_MEMORY_REQUEST) ||
            stack->Parameters.DeviceIoControl.OutputBufferLength == 0) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PREAD_MEMORY_REQUEST req = (PREAD_MEMORY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
        SIZE_T sizeToRead = req->size;
        if (sizeToRead > stack->Parameters.DeviceIoControl.OutputBufferLength)
            sizeToRead = stack->Parameters.DeviceIoControl.OutputBufferLength;

        PEPROCESS targetProc;
        status = PsLookupProcessByProcessId((HANDLE)req->pid, &targetProc);
        if (!NT_SUCCESS(status)) break;

        SIZE_T bytesRead = 0;
        status = MmCopyVirtualMemory(targetProc, (PVOID)req->address, PsGetCurrentProcess(), buffer, sizeToRead, KernelMode, &bytesRead);
        ObDereferenceObject(targetProc);

        if (NT_SUCCESS(status)) info = bytesRead;
        break;
    }
    case IOCTL_SEND_MOUSE_INPUT: {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(SEND_INPUT_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PSEND_INPUT_REQUEST input = (PSEND_INPUT_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        InjectRelativeMouseInput(input->deltaX, input->deltaY);
        status = STATUS_SUCCESS;
        info = 0;
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

//
// InitializeVirtualMouse: Create a virtual HID mouse device via VHF
//
NTSTATUS InitializeVirtualMouse(PDRIVER_OBJECT DriverObject) {
    VHF_CONFIG config = { 0 };
    config.ReportDescriptor = VirtualMouseReportDescriptor;
    config.ReportDescriptorLength = sizeof(VirtualMouseReportDescriptor);
    config.DeviceAttributes.VendorID = 0x1234;
    config.DeviceAttributes.ProductID = 0x5678;
    config.DeviceAttributes.VersionNumber = 0x0001;
    config.DeviceProductString = L"Virtual HID Mouse";
    config.DriverObject = DriverObject;

    NTSTATUS status = VhfCreateDevice(&config, &g_VhfDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(("VhfCreateDevice failed: 0x%X\n", status));
    }
    return status;
}

//
// InjectRelativeMouseInput: Send relative X, Y movement input report to virtual mouse
//
VOID InjectRelativeMouseInput(LONG dx, LONG dy) {
    if (!g_VhfDevice) return;

    // The input report matches the HID report descriptor:
    // 1 byte buttons (all zero), 1 byte padding, 1 byte X, 1 byte Y
    UCHAR report[4] = { 0 };
    report[2] = (UCHAR)dx; // X movement (signed 8-bit)
    report[3] = (UCHAR)dy; // Y movement (signed 8-bit)

    NTSTATUS status = VhfSendInputReport(g_VhfDevice, report, sizeof(report));
    if (!NT_SUCCESS(status)) {
        KdPrint(("VhfSendInputReport failed: 0x%X\n", status));
    }
}
