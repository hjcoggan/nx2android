#!/usr/bin/env python3
# Linux (arm64, glibc) build of the host: the shared host and graphics sources, the Darwin compatibility layer and the Vulkan backend of dm/android,
# with the Linux front end (SDL3: window, Vulkan surface, controllers, audio) of dm/linux in place of the NativeActivity one.
#   build.py --game <title ID> [--out <dir>] [--jobs N] [--only-compile] [--release]
# Runs on an arm64 Linux machine or container (dm/linux/Dockerfile: clang, lld, glslc, SDL3, Vulkan headers). Output: <out>/dmhost, an executable
# that runs the data pack porter makes for Android (mods/, RomFS/, shaders.dmsv): see dm/linux/README.md.
import argparse, concurrent.futures as cf, hashlib, os, re, shutil, struct, subprocess, sys
from pathlib import Path

DM = Path(__file__).resolve().parent.parent          # dm/
ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True)
ap.add_argument('--out')
ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
ap.add_argument('--only-compile', action='store_true', help='compile every source and report errors, no link')
ap.add_argument('--keep-going', action='store_true')
ap.add_argument('-O', default='2')
ap.add_argument('--release', action='store_true', help='no debug information, stripped, no DM_CTL')
ap.add_argument('--cc', default=os.environ.get('CC', 'clang'))
a = ap.parse_args()

CC = a.cc
out = Path(a.out) if a.out else DM / 'linux/out' / (a.game + ('-release' if a.release else ''))
DBG = [] if a.release else ['-g']
obj = out / 'obj'; gen = out / 'gen'; obj.mkdir(parents=True, exist_ok=True); (gen / a.game).mkdir(parents=True, exist_ok=True)

subprocess.run([sys.executable, str(DM / 'games/game.py'), 'header', a.game, str(gen / a.game / 'game_profile.h')], check=True)
subprocess.run([sys.executable, str(DM / 'android/tools/android_profile.py'), 'header', a.game, str(gen / a.game / 'android_profile.h')], check=True)

for sh in sorted((DM / 'android/shaders').glob('*.*')):   # the backend's own shaders, embedded as SPIR-V words
    hdr = gen / a.game / f'{sh.stem}_{sh.suffix[1:]}.spv.h'
    if hdr.exists() and hdr.stat().st_mtime >= sh.stat().st_mtime: continue
    spv = gen / f'{sh.name}.spv'
    subprocess.run(['glslc', '--target-env=vulkan1.1', '-O', str(sh), '-o', str(spv)], check=True)
    words = spv.read_bytes(); n = len(words) // 4
    body = ', '.join(f'{w:#010x}' for w in struct.unpack(f'<{n}I', words))
    hdr.write_text(f'// generated from dm/android/shaders/{sh.name} by dm/linux/build.py (glslc)\nstatic const uint32_t {sh.stem}_{sh.suffix[1:]}_spv[] = {{{body}}};\n')

def srcs(*globs): return [p for g in globs for p in sorted(DM.glob(g))]
# dm/android sources that talk to Android itself; dm/linux has their Linux counterparts
ANDROID_ONLY = {'front_android.c', 'perf_android.c', 'haptics_android.c', 'lan_android.c', 'gl_android.c', 'movie_avc.c', 'gfx_null.c',
                'audiotoolbox_aaudio.c', 'audioconverter_mediacodec.c'}
FIRST = DM / 'linux/tls_slot.c'   # first on the link line: its TLS variable must be the first of the executable's TLS block (tls_slot.c)
HOST = [p for p in srcs('host/*.c') if p.name not in ('hle_movie_avc.c',)]
GFX = srcs('gfx/*.c', 'gfx/nvn/*.c', 'gfx/tex/*.c')
PORT = [p for p in srcs('android/compat/*.c', 'android/*.c') if p.name not in ANDROID_ONLY] + [p for p in srcs('linux/*.c') if p != FIRST]
ASM = srcs('host/entry.S', 'gfx/nvn/nvn_entry.S', 'gfx/nvn/nvn_thunks.S')
VPX = srcs('third_party/libvpx/**/*.c')
ZSTD = srcs('third_party/zstd/zstddeclib.c')

INC = ['-include', str(DM / 'android/compat/include/darwin_compat.h'), f'-I{DM / "linux/include"}', f'-I{DM / "android/compat/include"}', f'-I{DM / "host"}', f'-I{DM / "gfx/nvn"}',
       f'-I{DM / "gfx/tex"}', f'-I{DM / "gfx/nvn/include"}', f'-I{DM / "third_party/vulkan-headers/include"}', f'-I{gen / a.game}', f'-I{DM / "third_party/libvpx"}',
       f'-I{DM / "third_party/stb_vorbis"}', f'-I{DM / "third_party/zstd"}', f'-I{DM / "android"}', f'-I{DM / "linux"}']
SDL_CFLAGS = subprocess.run(['pkg-config', '--cflags', 'sdl3'], capture_output=True, text=True).stdout.split()
SDL_LIBS = subprocess.run(['pkg-config', '--libs', 'sdl3'], capture_output=True, text=True).stdout.split() or ['-lSDL3']
CFLAGS = [f'-O{a.O}'] + DBG + ['-fPIC', '-fno-omit-frame-pointer', '-ffixed-x18', '-Wall', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
          '-Wno-format', '-Wno-deprecated-declarations', '-Wno-gnu-folding-constant', '-Werror=implicit-function-declaration', '-Werror=return-type',
          '-Werror=incompatible-pointer-types', '-Werror=int-conversion', '-D_GNU_SOURCE', '-DDM_LINUX=1', '-fno-strict-aliasing', '-fvisibility=hidden'] + SDL_CFLAGS \
         + (['-DDM_RELEASE=1'] if a.release else [])
VPX_FLAGS = ['-O2'] + DBG + ['-fPIC', f'-I{DM / "third_party/libvpx"}', f'-I{DM / "third_party/libvpx/config"}', '-w']
ZSTD_FLAGS = ['-O2'] + DBG + ['-fPIC', '-fvisibility=hidden', '-DZSTDLIB_VISIBLE=', '-DZSTDERRORLIB_VISIBLE=', '-w']

_stamp = obj / 'flags.stamp'; _flags = hashlib.sha256(repr((CFLAGS, INC, VPX_FLAGS, ZSTD_FLAGS, CC)).encode()).hexdigest()
if not _stamp.exists() or _stamp.read_text() != _flags:
    shutil.rmtree(obj, ignore_errors=True); obj.mkdir(parents=True, exist_ok=True); _stamp.write_text(_flags)

def dep_stale(o, d, s):
    if not o.exists() or o.stat().st_mtime < s.stat().st_mtime or not d.exists(): return True
    t = o.stat().st_mtime
    txt = d.read_text(errors='replace').replace('\\\n', ' ')
    for f in re.split(r'(?<!\\)\s+', txt.split(':', 1)[1] if ':' in txt else ''):
        f = f.replace('\\ ', ' ').strip()
        if f and Path(f).exists() and Path(f).stat().st_mtime > t: return True
    return False

def compile_one(s):
    rel = str(s.relative_to(DM)).replace(os.sep, '/'); o = obj / (rel.replace('/', '_') + '.o'); d = o.with_suffix('.d')
    src = s
    if s.suffix == '.S':   # Mach-O assembly → ELF
        src = gen / (s.stem + '.elf.S')
        if not src.exists() or src.stat().st_mtime < s.stat().st_mtime:
            subprocess.run([sys.executable, str(DM / 'android/tools/darwin2elf.py'), str(s), str(src)], check=True)
    if not dep_stale(o, d, src): return s, o, 0, ''
    flags = VPX_FLAGS if 'third_party/libvpx' in rel else ZSTD_FLAGS if 'third_party/zstd' in rel else DBG if s.suffix == '.S' else CFLAGS + INC
    if rel == 'host/main.c': flags = flags + ['-Dmain=dm_host_main']   # front_linux.c's main sets up the data pack and SDL, then runs the host on its thread
    r = subprocess.run([CC] + flags + ['-MMD', '-MF', str(d), '-c', str(src), '-o', str(o)], capture_output=True, text=True, errors='replace')
    return s, o, r.returncode, r.stderr

def build(files):
    objs, failed = [], []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for s, o, rc, err in ex.map(compile_one, files):
            objs.append(o)
            if rc: failed.append(s); print(f'--- {s.relative_to(DM)}\n{err.strip()}\n', file=sys.stderr)
    return objs, failed

all_files = [FIRST] + HOST + GFX + PORT + ASM
objs, failed = build(all_files)
vobjs, vfailed = build(VPX + ZSTD)
failed += vfailed
print(f'compiled {len(all_files) + len(VPX) + len(ZSTD)} sources, {len(failed)} failed' + (': ' + ', '.join(str(f.relative_to(DM)) for f in failed) if failed else ''))
if failed and not a.keep_going: sys.exit(1)
if a.only_compile: sys.exit(1 if failed else 0)

LIBS = SDL_LIBS + ['-lz', '-lm', '-ldl', '-lpthread']   # Vulkan: dlopen'd (gfx_vk_core.c vk_init)
LDFLAGS = ['-fuse-ld=lld', '-Wl,--error-limit=0', '-Wl,--build-id', '-Wl,-z,nostart-stop-gc', '-pie', '-rdynamic']
r = subprocess.run([CC] + LDFLAGS + ['-o', str(out / 'dmhost')] + [str(o) for o in objs + vobjs] + LIBS, capture_output=True, text=True, errors='replace')
print(r.stderr.strip() if r.returncode else f'linked {out / "dmhost"}', file=sys.stderr if r.returncode else sys.stdout)
if r.returncode: sys.exit(1)
if a.release:
    subprocess.run(['llvm-strip', '--strip-unneeded', str(out / 'dmhost')], check=True)
