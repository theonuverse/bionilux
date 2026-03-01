from graphviz import Digraph


def create_bionilux_diagram():
    dot = Digraph('bionilux_architecture', format='png')
    dot.attr(rankdir='TB', nodesep='0.6', ranksep='0.7', dpi='150')
    dot.attr('graph', label='bionilux v0.2.0 \u2014 Architecture',
             labelloc='t', fontsize='16', fontname='Arial Bold')
    dot.attr('node', shape='box', style='rounded,filled',
             fillcolor='#f9f9f9', fontname='Arial', fontsize='10')
    dot.attr('edge', fontname='Arial', fontsize='9')

    # User entry
    dot.node('user', 'User runs:\nbionilux ./program',
             shape='plaintext', fillcolor='none', fontsize='12')

    # bionilux main binary
    bionilux_label = (
        'bionilux  (native bionic binary)\n'
        '\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\n'
        '\u2022 Reads ELF header + PT_INTERP via bionilux_elf.h\n'
        '\u2022 Detects arch: aarch64 / x86_64\n'
        '\u2022 Detects libc: glibc / bionic / musl\n'
        '\u2022 Extracts embedded preload .so on first run\n'
        '\u2022 Builds sanitized environment (strips LD_AUDIT etc.)\n'
        '\u2022 Installs signal handlers BEFORE fork()\n'
        '\u2022 Acquires Termux wake-lock during execution'
    )
    dot.node('bionilux', bionilux_label,
             fillcolor='#e1f5fe', color='#01579b')

    # ELF header (shared)
    elf_label = (
        'bionilux_elf.h  (shared header)\n'
        '\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\n'
        '\u2022 elf_pread() \u2014 EINTR-safe pread wrapper\n'
        '\u2022 is_glibc_elf() \u2014 detect glibc interpreter\n'
        '\u2022 Used by BOTH bionilux.c and preload.c\n'
        '\u2022 static inline \u2192 compiled into each binary'
    )
    dot.node('elf_h', elf_label,
             fillcolor='#f3e5f5', color='#7b1fa2', shape='component')

    # Architecture detection
    dot.node('detect', 'ELF Architecture?',
             shape='diamond', fillcolor='#fff3e0', color='#e65100',
             fontsize='10')

    # ARM64 path
    dot.node('arm64_glibc', 'ARM64 glibc binary',
             fillcolor='#c8e6c9', color='#2e7d32')
    dot.node('arm64_bionic', 'ARM64 bionic binary',
             fillcolor='#e8eaf6', color='#283593')
    dot.node('loader', 'ld-linux-aarch64.so.1\n(glibc dynamic linker)',
             fillcolor='#c8e6c9', color='#2e7d32')

    # x86_64 path
    dot.node('x86', 'x86_64 binary',
             fillcolor='#ffccbc', color='#bf360c')
    dot.node('box64', 'box64  (x86_64 \u2192 ARM64 emulator)\n'
             '+ glibc loader wrapper script',
             fillcolor='#ffccbc', color='#bf360c')

    # Preload library
    preload_label = (
        'libbionilux_preload.so  (LD_PRELOAD into glibc process)\n'
        '\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\n'
        '\u2022 Hooks: execve execv execvp execvpe execl execlp execle\n'
        '\u2022 Hooks: readlink readlinkat (/proc/self/exe fix)\n'
        '\u2022 Redirects glibc children through loader automatically\n'
        '\u2022 Cleans env for bionic children (strips LD_AUDIT etc.)\n'
        '\u2022 dlsym NULL-safe: falls back to SYS_execve syscall'
    )
    dot.node('preload', preload_label,
             fillcolor='#fff9c4', color='#f9a825')

    # Child process
    dot.node('child', 'Child process runs\nwith correct libc + env',
             fillcolor='#e0f2f1', color='#00695c', shape='ellipse')

    # Direct exec (bionic)
    dot.node('direct', 'Direct execv()\n(no wrapping needed)',
             fillcolor='#e8eaf6', color='#283593', shape='ellipse')

    # Edges
    dot.edge('user', 'bionilux', label='  invokes')
    dot.edge('bionilux', 'elf_h', label='#include',
             style='dashed', color='#7b1fa2')
    dot.edge('bionilux', 'detect', label='analyze_binary()')

    dot.edge('detect', 'arm64_glibc', label='aarch64\n+ ld-linux')
    dot.edge('detect', 'arm64_bionic', label='aarch64\n+ linker64')
    dot.edge('detect', 'x86', label='x86_64')

    dot.edge('arm64_glibc', 'loader', label='--library-path\n--argv0')
    dot.edge('arm64_bionic', 'direct')
    dot.edge('x86', 'box64', label='BOX64_LD_LIBRARY_PATH\nBOX64_PATH')

    dot.edge('loader', 'preload', label='LD_PRELOAD')
    dot.edge('preload', 'elf_h', label='#include',
             style='dashed', color='#7b1fa2')
    dot.edge('preload', 'child', label='hooked exec*()')
    dot.edge('box64', 'child', label='emulated\nexecution')

    # Render
    dot.render('assets/bionilux_architecture_diagram', cleanup=True)
    print('Saved: assets/bionilux_architecture_diagram.png')


if __name__ == '__main__':
    create_bionilux_diagram()
