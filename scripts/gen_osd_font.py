"""
Generates the column-major glyph table for osd_font.c from a row-major
definition, so the table in the source is produced by code rather than
typed by hand.

This script is the source of truth for the glyph shapes. Editing
osd_font.c's table directly is fine for a one-off fix, but come back here
if you add several characters, because the transpose is the step that goes
wrong and doing it by hand once per glyph is how a font ends up with a
mirrored J.

Row-major glyphs are written as seven 5-character strings, top row first,
"#" for a set pixel. That is the form every published 5x7 reference uses,
so glyphs can be copied in verbatim and compared visually.

Usage:  python scripts/gen_osd_font.py > src/osd_font.c
"""

GLYPHS = {
    ' ': ["     ", "     ", "     ", "     ", "     ", "     ", "     "],
    '0': [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
    '1': ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    '2': [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
    '3': ["#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "],
    '4': ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
    '5': ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
    '6': ["  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "],
    '7': ["#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "],
    '8': [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
    '9': [" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "],
    'A': [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    'B': ["#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "],
    'C': [" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "],
    'D': ["###  ", "#  # ", "#   #", "#   #", "#   #", "#  # ", "###  "],
    'E': ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
    'F': ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
    'G': [" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"],
    'H': ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    'I': [" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    'J': ["    #", "    #", "    #", "    #", "#   #", "#   #", " ### "],
    'K': ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"],
    'L': ["#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
    'M': ["#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"],
    'N': ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
    'O': [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    'P': ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
    'Q': [" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"],
    'R': ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
    'S': [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
    'T': ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    'U': ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    'V': ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    'W': ["#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"],
    'X': ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
    'Y': ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "],
    'Z': ["#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"],
    '-': ["     ", "     ", "     ", "#####", "     ", "     ", "     "],
    '+': ["     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "],
    '.': ["     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "],
    ',': ["     ", "     ", "     ", "     ", " ##  ", " ##  ", " #   "],
    ':': ["     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "],
    '/': ["    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "],
    '%': ["##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"],
    '=': ["     ", "     ", "#####", "     ", "#####", "     ", "     "],
    '!': ["  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "],
    '?': [" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "],
    '(': ["   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "],
    ')': [" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "],
    '[': [" ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### "],
    ']': [" ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### "],
    '<': ["   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "],
    '>': [" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "],
    '*': ["     ", "# # #", " ### ", "#####", " ### ", "# # #", "     "],
    '_': ["     ", "     ", "     ", "     ", "     ", "     ", "#####"],
    # Degree sign. Small and raised, which is how it reads on a real
    # instrument display; a full-height 'o' would collide with the digits.
    '~': [" ##  ", "#  # ", "#  # ", " ##  ", "     ", "     ", "     "],
    # The two bar graphs used by the level/alarm indicator.
    '\x01': ["     ", "     ", "     ", "     ", "     ", "     ", "#####"],
}

# Aliases: characters we never render but that should map to something sane
# rather than to the placeholder box.
ALIASES = {
    '"': None, "'": None, ';': None, '\\': None, '^': None, '`': None,
    '{': '(', '}': ')', '|': '!',
}


def transpose(rows):
    """Row-major 7x5 -> column-major 5 bytes, bit 0 = top row."""
    cols = []
    for x in range(5):
        byte = 0
        for y in range(7):
            if rows[y][x] == '#':
                byte |= 1 << y
        cols.append(byte)
    return cols


def c_ident(ch):
    if ch == ' ':
        return 'SPACE'
    if ch == '\\x01':
        return 'BAR'
    names = {
        '-': 'MINUS', '+': 'PLUS', '.': 'DOT', ',': 'COMMA', ':': 'COLON',
        '/': 'SLASH', '%': 'PERCENT', '=': 'EQUAL', '!': 'BANG', '?': 'QUEST',
        '(': 'LPAREN', ')': 'RPAREN', '[': 'LBRACK', ']': 'RBRACK',
        '<': 'LT', '>': 'GT', '*': 'STAR', '_': 'UNDER',
        # The degree sign is stored as '~' because that is the ASCII character
        # closest to it that no OSD string will ever legitimately contain.
        '~': 'DEGREE',
    }
    if ch in names:
        return names[ch]
    if ch.isdigit():
        return 'DIGIT_' + ch
    return 'LETTER_' + ch


def main():
    out = []
    out.append('/*')
    out.append(' * Generated by scripts/gen_osd_font.py - do not edit the table by hand.')
    out.append(' *')
    out.append(' * The glyph shapes live in that script in row-major form, which is how')
    out.append(' * every published 5x7 reference writes them and how a human can check them')
    out.append(' * at a glance. The transpose into this file\'s column-major layout is what')
    out.append(' * the script is for: doing it by hand once per glyph is how a font ends up')
    out.append(' * with one mirrored character that nobody notices for a month.')
    out.append(' */')
    out.append('')
    out.append('#include "osd_font.h"')
    out.append('')
    out.append('/*')
    out.append(' * Column-major, bit 0 = top row. Five bytes per glyph, leftmost column')
    out.append(' * first. See osd_font.h for why this layout and not row-major.')
    out.append(' */')
    out.append('static const uint8_t kGlyphs[][OSD_FONT_WIDTH] = {')
    for ch, rows in GLYPHS.items():
        assert len(rows) == 7, repr(ch)
        for r in rows:
            assert len(r) == 5, repr(ch)
        cols = transpose(rows)
        hexes = ', '.join('0x%02X' % c for c in cols)
        label = c_ident(ch)
        shown = ch if ch.isprintable() and ch not in '\\' else '?'
        out.append('    /* %-8s (%-3s) */ { %s },' % ("'%s'" % shown, label, hexes))
    out.append('};')
    out.append('')
    out.append('/*')
    out.append(' * The lookup table. One entry per printable ASCII character, indexed by')
    out.append(' * (c - 32), giving an index into kGlyphs.')
    out.append(' *')
    out.append(' * Indexed rather than searched because the renderer asks for a glyph once')
    out.append(' * per character per frame, and a linear scan over sixty entries thirty')
    out.append(' * times a second is a silly amount of work for a font that never changes.')
    out.append(' *')
    out.append(' * Generated from the same table as the glyphs, so the two cannot drift. A')
    out.append(' * character the font does not have maps to the placeholder rather than')
    out.append(' * failing, and lowercase maps to the uppercase glyph - a 5x7 cell has no')
    out.append(' * room for real descenders, and pretending otherwise produces mush.')
    out.append(' */')
    out.append('static const uint8_t kAsciiIndex[96] = {')

    # Build the ASCII map.
    index = {}
    for i, ch in enumerate(GLYPHS):
        index[ch] = i
    placeholder = index['?']

    rows = []
    for code in range(32, 128):
        ch = chr(code)
        lookup = ch
        if lookup not in index:
            up = ch.upper()
            if up in index:
                lookup = up
            elif ch in ALIASES and ALIASES[ch] and ALIASES[ch] in index:
                lookup = ALIASES[ch]
            else:
                lookup = '?'
        rows.append((ch, index[lookup]))

    for i in range(0, 96, 8):
        chunk = rows[i:i + 8]
        entries = ', '.join('%2d' % v for _, v in chunk)
        # Show the source characters above the values so a wrong mapping is
        # visible without counting columns.
        labels = ' '.join('%2s' % (c if c != ' ' else "' '") for c, _ in chunk)
        out.append('    %s /* %s */' % (entries + ',', labels))
    out.append('};')
    out.append('')
    out.append('const struct osd_glyph *osd_font_lookup(char c)')
    out.append('{')
    out.append('    unsigned char uc = (unsigned char)c;')
    out.append('    int index;')
    out.append('')
    out.append('    if (uc < 32U || uc > 127U) {')
    out.append('        return NULL;')
    out.append('    }')
    out.append('')
    out.append('    index = kAsciiIndex[uc - 32U];')
    out.append('    return (const struct osd_glyph *)kGlyphs[index];')
    out.append('}')
    out.append('')
    out.append('size_t osd_font_text_width(const char *text)')
    out.append('{')
    out.append('    size_t glyphs = 0U;')
    out.append('')
    out.append('    if (text == NULL || text[0] == \'\\0\') {')
    out.append('        return 0U;')
    out.append('    }')
    out.append('')
    out.append('    /*')
    out.append('     * Every character advances, including ones the font does not know,')
    out.append('     * so a line\'s width does not change when a value happens to contain')
    out.append('     * an unsupported symbol. The trailing 1px separator is dropped.')
    out.append('     */')
    out.append('    while (text[glyphs] != \'\\0\') {')
    out.append('        glyphs++;')
    out.append('    }')
    out.append('')
    out.append('    return glyphs * (size_t)OSD_FONT_ADVANCE - 1U;')
    out.append('}')
    out.append('')
    out.append('size_t osd_font_line_height(void)')
    out.append('{')
    out.append('    return (size_t)OSD_FONT_HEIGHT;')
    out.append('}')
    out.append('')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
