# Setelah build, salin firmware ke folder bin/ dengan nama berisi versi,
# siap dipilih di aplikasi: Sistem > Update Firmware > Pilih file .bin
Import("env")
import os, re, shutil

def copy_bin(source, target, env):
    proj = env.subst("$PROJECT_DIR")
    src = os.path.join(proj, "src", "main.cpp")
    ver = "x"
    with open(src, encoding="utf-8") as f:
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
        if m:
            ver = m.group(1)
    name = env.subst("$PIOENV")
    suffix = "" if name == "esp32dev" else "-" + name
    out_dir = os.path.join(proj, "bin")
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "manggot-v%s%s.bin" % (ver, suffix))
    shutil.copyfile(str(target[0]), dst)
    print("\n[OTA] File firmware siap: %s\n" % dst)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_bin)
