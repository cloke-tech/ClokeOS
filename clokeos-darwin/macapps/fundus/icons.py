#!/usr/bin/env python3
"""Erzeugt die Icons von Fundus als SVG (Ordner im Finder-Stil mit eingeprägtem Symbol, Datei-Icons je Art,
Seitenleisten-Symbole). icons.py <ausgabeordner>; render.swift macht daraus PNGs."""
import os, sys

# Symbole in einem 100x100-Feld (Mitte 50,50), als Pfade/Formen ohne Füllfarbe (die setzt der Aufrufer)
GLYPH = {
    'home': '<path d="M50 22 L82 50 H73 V80 H58 V62 H42 V80 H27 V50 H18 Z"/>',
    'desktop': '<rect x="20" y="26" width="60" height="40" rx="5"/><rect x="42" y="66" width="16" height="9"/><rect x="33" y="75" width="34" height="6" rx="3"/>',
    'documents': '<path d="M30 18 H58 L72 32 V82 H30 Z"/><path d="M58 18 V32 H72" opacity=".5"/>',
    'downloads': '<circle cx="50" cy="50" r="31"/><path d="M44 30 H56 V50 H66 L50 68 L34 50 H44 Z" fill="#fff" fill-opacity=".85"/>',
    'movies': '<rect x="20" y="28" width="60" height="44" rx="5"/><g fill="#fff" fill-opacity=".85"><rect x="24" y="32" width="6" height="6"/><rect x="24" y="47" width="6" height="6"/><rect x="24" y="62" width="6" height="6"/><rect x="70" y="32" width="6" height="6"/><rect x="70" y="47" width="6" height="6"/><rect x="70" y="62" width="6" height="6"/></g>',
    'music': '<path d="M40 24 L74 18 V62 A10 9 0 1 1 66 54 V32 L46 36 V68 A10 9 0 1 1 38 60 Z"/>',
    'pictures': '<rect x="18" y="26" width="64" height="48" rx="6"/><g fill="#fff" fill-opacity=".85"><circle cx="34" cy="40" r="6"/><path d="M24 68 L42 50 L54 60 L64 50 L76 68 Z"/></g>',
    'library': '<path d="M18 30 L50 18 L82 30 Z"/><rect x="24" y="34" width="8" height="32"/><rect x="38" y="34" width="8" height="32"/><rect x="54" y="34" width="8" height="32"/><rect x="68" y="34" width="8" height="32"/><rect x="18" y="70" width="64" height="8"/>',
    'applications': '<path d="M44 18 H56 L80 80 H68 L62 64 H38 L32 80 H20 Z M42 54 H58 L50 32 Z" fill-rule="evenodd"/>',
    'public': '<circle cx="50" cy="34" r="13"/><path d="M24 80 C24 60 36 52 50 52 C64 52 76 60 76 80 Z"/>',
    'trash': '<rect x="26" y="30" width="48" height="52" rx="6"/><rect x="20" y="22" width="60" height="7" rx="3"/><rect x="42" y="15" width="16" height="7" rx="2"/>',
    'disk': '<rect x="16" y="34" width="68" height="34" rx="7"/><circle cx="72" cy="58" r="3.5" fill="#fff" fill-opacity=".9"/>',
    'volumes': '<rect x="16" y="26" width="68" height="20" rx="5"/><rect x="16" y="54" width="68" height="20" rx="5"/>',
    'nt': '<rect x="20" y="20" width="28" height="28" rx="2"/><rect x="52" y="20" width="28" height="28" rx="2"/><rect x="20" y="52" width="28" height="28" rx="2"/><rect x="52" y="52" width="28" height="28" rx="2"/>',
    'tux': '<ellipse cx="50" cy="56" rx="22" ry="26"/><circle cx="50" cy="30" r="14"/>',
    'root': '<rect x="16" y="34" width="68" height="34" rx="7"/>',
    'recent': '<circle cx="50" cy="50" r="30" fill="none" stroke-width="8"/><path d="M46 30 H54 V48 L66 58 L61 64 L46 52 Z"/>',
}

FOLDER_GLYPH = {'folder': None, 'home': 'home', 'desktop': 'desktop', 'documents': 'documents', 'downloads': 'downloads',
                'movies': 'movies', 'music': 'music', 'pictures': 'pictures', 'library': 'library',
                'applications': 'applications', 'public': 'public'}


def folder_svg(glyph):
    g = ''
    if glyph:
        g = f'<g transform="translate(64 78) scale(.42) translate(-50 -50)" fill="#3f6fd8" fill-opacity=".55">{GLYPH[glyph]}</g>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<defs>
<linearGradient id="b" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7fb0ff"/><stop offset="1" stop-color="#5a8ff0"/></linearGradient>
<linearGradient id="f" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#a8cbff"/><stop offset="1" stop-color="#78a9fb"/></linearGradient>
</defs>
<path d="M10 30 Q10 22 18 22 H48 Q53 22 56 26 L60 31 H110 Q118 31 118 39 V100 Q118 108 110 108 H18 Q10 108 10 100 Z" fill="url(#b)"/>
<path d="M10 44 Q10 38 16 38 H112 Q118 38 118 44 V102 Q118 110 110 110 H18 Q10 110 10 102 Z" fill="url(#f)"/>
<path d="M10.5 44 Q10.5 38.5 16 38.5 H112 Q117.5 38.5 117.5 44" fill="none" stroke="#fff" stroke-opacity=".55"/>
{g}</svg>'''


def doc_svg(label, color, glyph=None):
    g = ''
    if glyph:
        g = f'<g transform="translate(64 66) scale(.36) translate(-50 -50)" fill="{color}">{GLYPH[glyph]}</g>'
    lab = ''
    if label:
        lab = (f'<rect x="30" y="86" width="68" height="18" rx="4" fill="{color}"/>'
               f'<text x="64" y="99.5" font-family="Helvetica, Arial, sans-serif" font-size="12" font-weight="700" fill="#fff" text-anchor="middle">{label}</text>')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<defs><linearGradient id="p" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#eef0f6"/></linearGradient></defs>
<path d="M28 10 H80 L104 34 V114 Q104 118 100 118 H28 Q24 118 24 114 V14 Q24 10 28 10 Z" fill="url(#p)" stroke="#c9cdd8" stroke-width="1.5"/>
<path d="M80 10 V30 Q80 34 84 34 H104" fill="#e3e6ee" stroke="#c9cdd8" stroke-width="1.5"/>
{g}{lab}</svg>'''


def app_svg():
    return '''<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<defs><linearGradient id="a" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#9b7bff"/><stop offset="1" stop-color="#4b23c8"/></linearGradient></defs>
<rect x="12" y="12" width="104" height="104" rx="24" fill="url(#a)"/>
<g transform="translate(64 64) scale(.62) translate(-50 -50)" fill="#fff" fill-opacity=".92">''' + GLYPH['applications'] + '</g></svg>'


def disk_svg():
    return '''<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<defs><linearGradient id="d" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4f5f8"/><stop offset="1" stop-color="#b9bec9"/></linearGradient></defs>
<rect x="10" y="40" width="108" height="52" rx="10" fill="url(#d)" stroke="#9aa0ad" stroke-width="1.5"/>
<rect x="10" y="74" width="108" height="18" rx="6" fill="#8d93a0" fill-opacity=".35"/>
<circle cx="102" cy="83" r="3.5" fill="#46d16a"/></svg>'''


def trash_svg(full):
    paper = '<path d="M38 36 L54 22 L70 34 L86 24 L92 40 Z" fill="#fff" stroke="#c9cdd8"/>' if full else ''
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<defs><linearGradient id="t" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#e9ecf2"/><stop offset=".5" stop-color="#fbfcfe"/><stop offset="1" stop-color="#d9dde6"/></linearGradient></defs>
{paper}<path d="M28 36 H100 L94 112 Q93 118 87 118 H41 Q35 118 34 112 Z" fill="url(#t)" stroke="#aeb4c0" stroke-width="1.5"/>
<g stroke="#b8bec9" stroke-width="2"><path d="M48 46 L50 108"/><path d="M64 46 V108"/><path d="M80 46 L78 108"/></g>
<ellipse cx="64" cy="36" rx="36" ry="5" fill="#cfd4dd" stroke="#aeb4c0"/></svg>'''


def glyph_svg(name):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 100 100">'
            f'<g fill="#7c5cff" stroke="#7c5cff">{GLYPH[name]}</g></svg>')


def main(out):
    os.makedirs(out, exist_ok=True)
    files = {}
    for name, g in FOLDER_GLYPH.items():
        files[f'folder-{name}'] = folder_svg(g)
    kinds = {
        'file': ('', '#8a90a0', None), 'text': ('TXT', '#7a8296', None), 'code': ('CODE', '#5b6bd6', None),
        'image': ('', '#e0558a', 'pictures'), 'audio': ('', '#e0558a', 'music'), 'video': ('', '#6a55e0', 'movies'),
        'pdf': ('PDF', '#d64545', None), 'archive': ('ZIP', '#8a7a5c', None), 'diskimage': ('DMG', '#7a8296', 'disk'),
        'exe': ('EXE', '#3a7bd5', 'nt'), 'document': ('DOC', '#2f7de1', None), 'package': ('', '#7a5cd6', 'library'),
        'executable': ('EXEC', '#2b2f3a', None), 'alias': ('', '#8a90a0', None),
    }
    for k, (lab, col, g) in kinds.items():
        files[f'file-{k}'] = doc_svg(lab, col, g)
    files['app'] = app_svg()
    files['disk'] = disk_svg()
    files['trash-empty'] = trash_svg(False)
    files['trash-full'] = trash_svg(True)
    for g in GLYPH:
        files[f'side-{g}'] = glyph_svg(g)
    for name, svg in files.items():
        with open(os.path.join(out, name + '.svg'), 'w') as f:
            f.write(svg)
    print(len(files), 'Icons')


if __name__ == '__main__':
    main(sys.argv[1])
