from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent
default = root.parent / 'toolchain/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64'
toolchain = Path(os.environ.get('NDK_TOOLCHAIN', default))
compiler = toolchain / 'bin/clang.exe'
for helper in ('service-helper', 'property-helper'):
    output = root / f'app/src/main/assets/arm64-v8a/{helper}'
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(compiler), '--target=aarch64-linux-android26',
                    f'--sysroot={toolchain / "sysroot"}', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / f'{helper}.c'), '-o', str(output)], check=True)
    print(output)
