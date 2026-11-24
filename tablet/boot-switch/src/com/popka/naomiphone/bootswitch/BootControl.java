package com.popka.naomiphone.bootswitch;

import java.lang.reflect.InvocationTargetException;

// Runs through app_process in the root shell when the firmware has no bootctl.
// HwBinder is a platform API. Keep it out of the launcher app's SDK dependencies.
public final class BootControl {
    private static final String V1_0 = "android.hardware.boot@1.0::IBootControl";
    private static final String V1_1 = "android.hardware.boot@1.1::IBootControl";
    private static final String V1_2 = "android.hardware.boot@1.2::IBootControl";

    private BootControl() {}

    public static void main(String[] args) {
        try {
            execute(args);
        } catch (ReflectiveOperationException | RuntimeException error) {
            Throwable detail = error instanceof InvocationTargetException ? error.getCause() : error;
            System.err.println("Boot Control: " + detail);
            System.exit(1);
        }
    }

    private static void execute(String[] args) throws ReflectiveOperationException {
        if (android.os.Process.myUid() != 0) {
            throw new SecurityException("Boot Control requires root");
        }
        if (args.length == 0 || args.length > 2) {
            throw new IllegalArgumentException("expected a Boot Control command");
        }
        String command = args[0];
        // HIDL transaction numbers follow the method order in IBootControl.hal.
        int method = transactionCode(command);
        boolean takesSlot = method == 4 || method == 6 || method == 8;
        if (takesSlot ? args.length != 2 || !args[1].equals("1") : args.length != 1) {
            throw new IllegalArgumentException("expected the Ubuntu Touch slot _b");
        }

        Class<?> hwBinder = Class.forName("android.os.HwBinder");
        java.lang.reflect.Method getService = hwBinder.getMethod("getService", String.class, String.class, boolean.class);
        String halVersion = V1_2;
        Object service = null;
        try {
            service = getService.invoke(null, V1_2, "default", false);
        } catch (InvocationTargetException ignored) {
            service = null;
        }
        if (service == null) {
            halVersion = V1_1;
            service = getService.invoke(null, V1_1, "default", false);
        }
        if (service == null) {
            throw new IllegalStateException("HAL service is unavailable");
        }
        if (method == 0) {
            System.out.println("HAL Version: " + halVersion);
            return;
        }
        if (method == 11 && !V1_2.equals(halVersion)) {
            throw new UnsupportedOperationException("getActiveBootSlot requires Boot Control 1.2");
        }

        Class<?> parcel = Class.forName("android.os.HwParcel");
        Object request = parcel.getConstructor().newInstance();
        Object reply = parcel.getConstructor().newInstance();
        try {
            String token = method == 11 ? V1_2 : (method == 10 ? V1_1 : V1_0);
            parcel.getMethod("writeInterfaceToken", String.class).invoke(request, token);
            if (takesSlot) {
                parcel.getMethod("writeInt32", int.class).invoke(request, 1);
            }
            Class.forName("android.os.IHwBinder").getMethod("transact", int.class, parcel, parcel, int.class)
                    .invoke(service, method, request, reply, 0);
            parcel.getMethod("verifySuccess").invoke(reply);
            switch (method) {
                case 1:
                case 2:
                case 11:
                    System.out.println(parcel.getMethod("readInt32").invoke(reply));
                    break;
                case 4:
                    Object result = parcel.getMethod("readBuffer", long.class).invoke(reply, 24L);
                    if (!(boolean) result.getClass().getMethod("getBool", long.class).invoke(result, 0L)) {
                        String msg = "";
                        try {
                            Object raw = result.getClass().getMethod("getString", long.class).invoke(result, 8L);
                            if (raw instanceof String && !((String) raw).isEmpty()) {
                                msg = ": " + raw;
                            }
                        } catch (ReflectiveOperationException | RuntimeException ignored) {}
                        throw new IllegalStateException("HAL rejected setActiveBootSlot" + msg);
                    }
                    break;
                case 6:
                    if ((int) parcel.getMethod("readInt32").invoke(reply) != 1) {
                        throw new IllegalStateException("slot _b is not bootable");
                    }
                    break;
                case 8:
                    System.out.println(parcel.getMethod("readString").invoke(reply));
                    break;
                case 10:
                    System.out.println(mergeStatus((byte) parcel.getMethod("readInt8").invoke(reply)));
                    break;
                default:
                    throw new AssertionError(method);
            }
        } finally {
            parcel.getMethod("releaseTemporaryStorage").invoke(request);
            parcel.getMethod("release").invoke(request);
            parcel.getMethod("release").invoke(reply);
        }
    }

    private static int transactionCode(String command) {
        switch (command) {
            case "hal-info": return 0;
            case "get-number-slots": return 1;
            case "get-current-slot": return 2;
            case "set-active-boot-slot": return 4;
            case "is-slot-bootable": return 6;
            case "get-suffix": return 8;
            case "get-snapshot-merge-status": return 10;
            case "get-active-boot-slot": return 11;
            default: throw new IllegalArgumentException("unsupported command: " + command);
        }
    }

    private static String mergeStatus(byte status) {
        switch (status) {
            case 0: return "none";
            case 2: return "snapshotted";
            case 3: return "merging";
            case 4: return "cancelled";
            default: return "unknown";
        }
    }
}
