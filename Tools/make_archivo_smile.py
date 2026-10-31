# Gera Editor/Fonts/ArchivoSmile.ttf a partir da Archivo variavel do google/fonts (SIL OFL):
#   py -m pip install fonttools
#   py Tools/make_archivo_smile.py "Archivo[wdth,wght].ttf" Editor/Fonts/ArchivoSmile.ttf
#
# - wdth fixo em 88 (corte SemiCondensed): mais texto por linha nos docks de ~300 px.
# - wght segue variavel em 400..700 (Regular/Medium/DemiBold/Bold do QML).
# - Algarismos TABULARES viram o padrao do cmap: numeros alinham em coluna e nao "dancam" de
#   largura a cada frame sem precisar de font.features em cada Text.
# - Familia renomeada para "Archivo Smile" p/ nao colidir com uma Archivo instalada no sistema.
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

src, dst = sys.argv[1], sys.argv[2]
font = instancer.instantiateVariableFont(TTFont(src), {'wdth': 88, 'wght': (400, 700)})

order = set(font.getGlyphOrder())
digits = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']
suffix = next(s for s in ('.tf', '.tnum') if all(d + s in order for d in digits))
for table in font['cmap'].tables:
    if table.isUnicode():
        for i, d in enumerate(digits):
            if 0x30 + i in table.cmap:
                table.cmap[0x30 + i] = d + suffix

family, postscript = 'Archivo Smile', 'ArchivoSmile'
names = font['name']
for rec in list(names.names):
    ids = (rec.platformID, rec.platEncID, rec.langID)
    if rec.nameID in (1, 4, 16):
        names.setName(family, rec.nameID, *ids)
    elif rec.nameID == 6:
        names.setName(postscript + '-Regular', rec.nameID, *ids)
    elif rec.nameID == 25:
        names.setName(postscript, rec.nameID, *ids)
    elif rec.nameID == 3:
        names.setName(postscript + ';' + str(rec), rec.nameID, *ids)
if 'DSIG' in font:
    del font['DSIG']  # assinatura do arquivo original deixa de valer depois da edicao
font.save(dst)
print(f'{dst}: {family}, digitos {suffix}')
