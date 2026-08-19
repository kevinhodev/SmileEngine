# Textura lunar — fonte e processamento

## Arquivo usado pela Smile

- Arquivo no repositorio: `moon_lroc_color_2k.jpg`
- Arquivo original: `lroc_color_2k.jpg`
- Resolucao: 2048x1024, projecao equiretangular centrada em 0 grau de longitude
- SHA-256: `f7130a1822681fa7512d7dcfd40db8c10b9ba4f06777910348698260ed7a2170`
- Fonte direta: NASA Scientific Visualization Studio, CGI Moon Kit (ID 4720)
  - pagina: https://svs.gsfc.nasa.gov/4720/
  - download: https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/lroc_color_2k.jpg
- Importacao na Smile: download direto e renomeacao; sem redimensionamento ou re-encode. O hash
  acima foi conferido contra o download oficial em 2026-08-18.

O mapa foi preparado pelo NASA SVS para renderizacao. Ele deriva do mosaico de cor WAC
normalizado fotometricamente por Hapke, produzido pela equipe da Lunar Reconnaissance Orbiter
Camera a partir de mais de 100 mil imagens. O SVS compoe RGB com as bandas de 643, 566 e 415 nm,
ajusta exposicao e white balance para aproximar a visao humana e preenche as regioes polares sem
cobertura WAC com o mapa de albedo LDAM do LOLA. Por isso este JPEG e um asset visual, nao um
produto cientifico bruto.

Fonte cientifica do mosaico:

- https://data.lroc.im-ldi.com/lroc/view_rdr/WAC_HAPKE
- Sato et al. (2014), *Resolved Hapke parameter maps of the Moon*, JGR Planets 119,
  1775-1805, DOI: 10.1002/2013JE004580.

## Tratamento na engine

- A textura e interpretada como sRGB; o SRV entrega albedo linear ao shader.
- A cadeia de mips e filtrada em linear e reencodada em sRGB pela carga de textura.
- O shader projeta o mapa numa esfera, com longitude 0 no centro da face proxima e norte lunar
  no topo. Iluminacao, fase e limbo sao calculados em tempo real; o JPEG nao fornece normais nem
  os nove mapas de parametros Hapke.

## Credito e uso

Credito solicitado pela pagina do asset: **NASA's Scientific Visualization Studio**.

- Visualizacao: Ernie Wright (USRA)
- Cientista: Noah Petro (NASA/GSFC)
- Dados: NASA/GSFC/Arizona State University, Lunar Reconnaissance Orbiter Camera (LROC), e
  Lunar Orbiter Laser Altimeter (LOLA)

As midias da NASA em geral nao estao sujeitas a copyright nos Estados Unidos, mas o uso deve
seguir as regras de atribuicao e nao pode sugerir endosso da NASA:
https://www.nasa.gov/nasa-brand-center/images-and-media/
