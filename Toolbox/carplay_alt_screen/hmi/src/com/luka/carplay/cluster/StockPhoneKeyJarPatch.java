package com.luka.carplay.cluster;

import java.nio.file.Files;
import java.nio.file.Paths;
import jdk.internal.org.objectweb.asm.*;

/** JDK8 build tool, never packaged on the vehicle. Prepend a guard to the stock
 * CarplayDSILifecycleController$CarplayDSIController.postDSICarplayButtonEvent so that,
 * while a gated CarPlay call exists, the duplicate stock MFL phone HID key is dropped
 * and only the VC's iAP2 accept/hangup acts (see GatedPhoneService.dropStockPhoneKey).
 * Everything else in the OEM class is preserved byte-for-byte; only one method gains a
 * two-instruction prefix. Compile with javac -XDignore.symbol.file, then pass the stock
 * class path (args[0]) and the output class path (args[1]).
 */
public final class StockPhoneKeyJarPatch {
    public static void main(String[] args) throws Exception {
        final String svc = "com/luka/carplay/telephony/GatedPhoneService";
        final String method = "postDSICarplayButtonEvent";
        final String desc = "(II)V";
        ClassReader reader = new ClassReader(Files.readAllBytes(Paths.get(args[0])));
        /* Refuse a double patch: the guard must not already be present. */
        final int[] existing = {0};
        reader.accept(new ClassVisitor(Opcodes.ASM5) {
            public MethodVisitor visitMethod(int a, String n, String d, String s, String[] e) {
                if (!n.equals(method) || !d.equals(desc)) return null;
                return new MethodVisitor(Opcodes.ASM5) {
                    public void visitMethodInsn(int o, String owner, String name, String dc, boolean itf) {
                        if (owner.equals(svc) && name.equals("dropStockPhoneKey")) existing[0]++;
                    }
                };
            }
        }, 0);
        if (existing[0] != 0) throw new IllegalStateException("phone-key patch already present");
        final int[] methods = {0}, inserted = {0};
        ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
        reader.accept(new ClassVisitor(Opcodes.ASM5, writer) {
            public MethodVisitor visitMethod(int a, String n, String d, String s, String[] e) {
                MethodVisitor target = super.visitMethod(a, n, d, s, e);
                if (!n.equals(method) || !d.equals(desc)) return target;
                methods[0]++;
                return new MethodVisitor(Opcodes.ASM5, target) {
                    public void visitCode() {
                        super.visitCode();
                        /* if (GatedPhoneService.dropStockPhoneKey(i)) return; */
                        Label cont = new Label();
                        super.visitVarInsn(Opcodes.ILOAD, 1);   /* arg i (slot 0 = this) */
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, svc, "dropStockPhoneKey", "(I)Z", false);
                        super.visitJumpInsn(Opcodes.IFEQ, cont);
                        super.visitInsn(Opcodes.RETURN);
                        super.visitLabel(cont);
                        inserted[0]++;
                    }
                };
            }
        }, 0);
        if (methods[0] != 1 || inserted[0] != 1) throw new IllegalStateException("unexpected OEM method shape");
        Files.write(Paths.get(args[1]), writer.toByteArray());
        System.out.println("PHONE_KEY_PATCH=PASS method=postDSICarplayButtonEvent guard=dropStockPhoneKey OEM_methods_preserved=1");
    }
}
