from graphviz import Digraph

def create_ovgl_diagram():
    # 'dot' für klassische Hierarchien
    dot = Digraph('ovgl_architecture', format='png')
    dot.attr(rankdir='TB', nodesep='0.5', ranksep='0.5')
    
    # Standard-Styling für die Boxen
    dot.attr('node', shape='box', style='rounded,filled', fillcolor='#f9f9f9', fontname='Arial', fontsize='10')

    # Hauptknoten definieren
    dot.node('user', 'User runs: ovgl ./program', shape='plaintext', fillcolor='none')
    
    # Große ovgl Box (Main Logic)
    ovgl_main_label = (
        'ovgl (native bionic binary)\n'
        '• Analyzes ELF header to detect architecture\n'
        '• arm64 glibc -> invokes glibc loader\n'
        '• x86_64 -> invokes box64\n'
        '• Sets environment variables and preload library'
    )
    dot.node('ovgl', ovgl_main_label, fillcolor='#e1f5fe', color='#01579b')

    # Pfad Weichen
    dot.node('arm64', 'ARM64 glibc\nbinary')
    dot.node('x86', 'x86_64 binary')
    
    dot.node('loader', 'ld-linux.so.1\nloader')
    dot.node('box64', 'box64\n(emulator)')

    # Preload Box
    preload_label = (
        'libovgl_preload.so (loaded into process)\n'
        '• Hooks execve() to rewrite child process execution\n'
        '• Hooks readlink() to fix /proc/self/exe\n'
        '• Cleans environment for bionic binaries'
    )
    dot.node('preload', preload_label, fillcolor='#fff9c4', color='#fbc02d')

    # Verbindungen (Edges)
    dot.edge('user', 'ovgl')
    dot.edge('ovgl', 'arm64')
    dot.edge('ovgl', 'x86')
    
    dot.edge('arm64', 'loader')
    dot.edge('x86', 'box64')
    
    dot.edge('loader', 'preload')
    dot.edge('box64', 'preload')

    # Speichern und anzeigen
    dot.render('ovgl_architecture_diagram', view=True)

if __name__ == "__main__":
    create_ovgl_diagram()
